#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that a linked RESENTMENT image is actually bootable.

A kernel that links is not a kernel that boots. Every check here corresponds to
something a bootloader or a CPU will silently refuse to do, producing a blank
screen rather than a diagnostic:

  - the Multiboot2 header must be 8-byte aligned inside the first 32 KiB, or
    GRUB will not recognise the file as a kernel at all
  - its checksum must make the four header dwords sum to zero
  - the entry point must be reachable before paging exists, so below 4 GiB
  - the load segments must not overlap and must be page aligned
  - the higher-half symbols must actually be higher-half

Run it on every build. It costs milliseconds and catches the class of mistake
that otherwise costs an afternoon with a serial cable.
"""
import struct
import sys

MB2_MAGIC = 0xE85250D6


def read_elf(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{path}: not an ELF file")
    if data[4] != 2:
        raise SystemExit(f"{path}: not 64-bit")

    entry, phoff, shoff = struct.unpack_from("<QQQ", data, 0x18)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)

    phdrs = []
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<IIQQQQQQ", data, off)
        phdrs.append(dict(type=p_type, flags=p_flags, offset=p_offset,
                          vaddr=p_vaddr, paddr=p_paddr, filesz=p_filesz,
                          memsz=p_memsz, align=p_align))

    sections = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name, stype, flags, addr, foff, size = struct.unpack_from("<IIQQQQ", data, off)
        sections.append(dict(name=name, type=stype, flags=flags, addr=addr,
                             offset=foff, size=size))
    strtab = sections[shstrndx]
    for s in sections:
        end = data.index(b"\0", strtab["offset"] + s["name"])
        s["sname"] = data[strtab["offset"] + s["name"]:end].decode()

    return data, entry, phdrs, sections


def check_multiboot2(data, problems, notes):
    """Find and validate the Multiboot2 header."""
    limit = min(len(data), 32768)
    for off in range(0, limit - 16, 8):
        magic, arch, length, checksum = struct.unpack_from("<IIII", data, off)
        if magic != MB2_MAGIC:
            continue
        total = (magic + arch + length + checksum) & 0xFFFFFFFF
        if total != 0:
            problems.append(
                f"Multiboot2 header at {off:#x} has a bad checksum: "
                f"the four dwords sum to {total:#x}, not 0")
        else:
            notes.append(f"Multiboot2 header at offset {off:#x}, "
                         f"length {length}, checksum valid")
        if off % 8:
            problems.append(f"Multiboot2 header at {off:#x} is not 8-byte aligned")

        # Walk the tags so a malformed one is caught here rather than by GRUB.
        pos = off + 16
        end = off + length
        seen = []
        while pos + 8 <= end:
            ttype, tflags, tsize = struct.unpack_from("<HHI", data, pos)
            if tsize < 8:
                problems.append(f"Multiboot2 tag at {pos:#x} has size {tsize}")
                break
            seen.append(ttype)
            if ttype == 0:
                break
            pos += (tsize + 7) & ~7
        if 0 not in seen:
            problems.append("Multiboot2 header has no end tag")
        notes.append(f"Multiboot2 tags present: {sorted(set(seen))}")
        return True
    problems.append("no Multiboot2 header found in the first 32 KiB; "
                    "GRUB will not recognise this file")
    return False


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "dist/x86_64/resentment.elf"
    data, entry, phdrs, sections = read_elf(path)

    problems, notes = [], []
    notes.append(f"image: {path} ({len(data)} bytes)")
    notes.append(f"entry point: {entry:#x}")

    check_multiboot2(data, problems, notes)

    if entry >= (1 << 32):
        problems.append(f"entry point {entry:#x} is above 4 GiB; the 32-bit "
                        f"boot stub cannot be reached")

    loads = [p for p in phdrs if p["type"] == 1]
    if not loads:
        problems.append("no PT_LOAD segments; there is nothing to load")

    for i, p in enumerate(loads):
        notes.append(
            f"segment {i}: vaddr {p['vaddr']:#018x} paddr {p['paddr']:#012x} "
            f"file {p['filesz']:#x} mem {p['memsz']:#x} flags {p['flags']:#x}")
        if p["paddr"] % 4096:
            problems.append(f"segment {i} physical address {p['paddr']:#x} "
                            f"is not page aligned")
        if p["memsz"] < p["filesz"]:
            problems.append(f"segment {i} memsz is smaller than filesz")

    for i in range(len(loads)):
        for j in range(i + 1, len(loads)):
            a, b = loads[i], loads[j]
            a0, a1 = a["paddr"], a["paddr"] + a["memsz"]
            b0, b1 = b["paddr"], b["paddr"] + b["memsz"]
            if a0 < b1 and b0 < a1:
                problems.append(f"segments {i} and {j} overlap in physical memory")

    named = {s["sname"]: s for s in sections}
    for want in (".boot", ".text", ".rodata", ".bss"):
        if want not in named:
            problems.append(f"section {want} is missing")

    if ".boot" in named and named[".boot"]["addr"] >= (1 << 32):
        problems.append(".boot is linked high; the 32-bit stub cannot run there")
    if ".text" in named and named[".text"]["addr"] < 0xFFFFFFFF80000000:
        problems.append(".text is not in the higher half; the user/kernel "
                        "split will not work")

    total_mem = sum(p["memsz"] for p in loads)
    notes.append(f"resident size: {total_mem / 1024:.1f} KiB")

    for n in notes:
        print(f"  {n}")
    if problems:
        print()
        for p in problems:
            print(f"  PROBLEM: {p}")
        print(f"\n{len(problems)} problem(s) found")
        return 1
    print("\nimage looks bootable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
