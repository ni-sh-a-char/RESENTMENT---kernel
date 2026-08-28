#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Repackage the kernel as a 32-bit ELF for direct QEMU boot.

QEMU's own `-kernel` loader implements Multiboot 1 and refuses a 64-bit ELF:

    Cannot load x86-64 image, give a 32bit one.

GRUB has no such limitation, so the ELF64 stays the real artifact. This
produces a second file containing exactly the same bytes at exactly the same
physical addresses, wrapped in 32-bit headers, so that `qemu -kernel` can boot
it without an ISO, a bootloader or a disk image.

Nothing about the kernel changes. The loader reads segments at their physical
address, and the physical addresses are identical; the 64-bit virtual addresses
the kernel actually runs at are established by its own page tables a few
hundred instructions later, not by the ELF headers.

Usage:
    python tools/mkmb1.py dist/x86_64/resentment.elf dist/x86_64/resentment32.elf
"""
import struct
import sys

PT_LOAD = 1
EI_NIDENT = 16


def read_elf64(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"{path}: not a 64-bit ELF")

    entry, phoff = struct.unpack_from("<QQ", data, 0x18)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)

    segments = []
    for i in range(phnum):
        off = phoff + i * phentsize
        (p_type, p_flags, p_offset, p_vaddr, p_paddr,
         p_filesz, p_memsz, p_align) = struct.unpack_from("<IIQQQQQQ", data, off)
        if p_type != PT_LOAD:
            continue
        segments.append(dict(flags=p_flags, offset=p_offset, paddr=p_paddr,
                             filesz=p_filesz, memsz=p_memsz, align=p_align,
                             data=data[p_offset:p_offset + p_filesz]))
    return entry, segments


def write_elf32(path, entry, segments):
    ehsize = 52
    phentsize = 32
    phoff = ehsize
    data_start = phoff + phentsize * len(segments)

    # Keep each segment's file offset congruent to its physical address modulo
    # the page size. Some loaders mmap rather than read, and a mismatch there
    # is a silent corruption rather than an error.
    body = bytearray()
    placed = []
    cursor = data_start
    for s in segments:
        align = max(s["align"], 1)
        want = s["paddr"] % align if align > 1 else 0
        pad = (want - (cursor % align)) % align
        body += b"\0" * pad
        cursor += pad
        placed.append((cursor, s))
        body += s["data"]
        cursor += len(s["data"])

    if entry >= (1 << 32):
        raise SystemExit(f"entry point {entry:#x} does not fit in 32 bits")

    eh = bytearray(ehsize)
    eh[0:4] = b"\x7fELF"
    eh[4] = 1          # ELFCLASS32
    eh[5] = 1          # ELFDATA2LSB
    eh[6] = 1          # EV_CURRENT
    eh[7] = 0          # System V ABI
    struct.pack_into("<HHIIIIIHHHHHH", eh, 16,
                     2,            # ET_EXEC
                     3,            # EM_386
                     1,            # version
                     entry,
                     phoff,
                     0,            # no section headers
                     0,            # flags
                     ehsize, phentsize, len(segments),
                     0, 0, 0)

    ph = bytearray()
    for offset, s in placed:
        if s["paddr"] >= (1 << 32):
            raise SystemExit(f"segment at {s['paddr']:#x} does not fit in 32 bits")
        # vaddr is set equal to paddr: the loader places by physical address,
        # and a truncated 64-bit virtual address would only mislead a reader.
        ph += struct.pack("<IIIIIIII", PT_LOAD, offset, s["paddr"], s["paddr"],
                          len(s["data"]), s["memsz"], s["flags"],
                          min(s["align"], 0x1000))

    with open(path, "wb") as f:
        f.write(bytes(eh) + bytes(ph) + bytes(body))

    total = sum(s["memsz"] for _, s in placed)
    print(f"  MB1     {path}  entry {entry:#x}, {len(placed)} segments, "
          f"{total / 1024:.1f} KiB resident")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    entry, segments = read_elf64(sys.argv[1])
    if not segments:
        raise SystemExit("no PT_LOAD segments to repackage")
    write_elf32(sys.argv[2], entry, segments)
    return 0


if __name__ == "__main__":
    sys.exit(main())
