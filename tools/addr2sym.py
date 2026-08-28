#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Resolve a kernel address to the symbol containing it.

A panic prints an instruction pointer. Without a way to turn that back into a
function name, the message is a number. This is the smallest tool that closes
that loop, and it needs no debugger and no toolchain.

Usage:
    python tools/addr2sym.py dist/x86_64/resentment.elf 0xffffffff8013ef93 ...
"""
import struct
import sys


def symbols(path):
    data = open(path, "rb").read()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)

    sections = []
    for i in range(shnum):
        o = shoff + i * shentsize
        name, stype, flags, addr, off, size, link, info, align, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, o)
        sections.append(dict(name=name, type=stype, off=off, size=size,
                             link=link, entsize=entsize))

    out = []
    for s in sections:
        if s["type"] not in (2, 11) or not s["entsize"]:   # SYMTAB, DYNSYM
            continue
        strtab = sections[s["link"]]
        n = s["size"] // s["entsize"]
        for i in range(n):
            o = s["off"] + i * s["entsize"]
            st_name, st_info, st_other, st_shndx, st_value, st_size = \
                struct.unpack_from("<IBBHQQ", data, o)
            if not st_value:
                continue
            end = data.index(b"\0", strtab["off"] + st_name)
            name = data[strtab["off"] + st_name:end].decode("utf-8", "replace")
            if name:
                out.append((st_value, st_size, name))
    out.sort()
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    syms = symbols(sys.argv[1])
    if not syms:
        print("no symbol table in that image; link without -s")
        return 1

    for arg in sys.argv[2:]:
        addr = int(arg, 0)
        best = None
        for value, size, name in syms:
            if value <= addr and (size == 0 or addr < value + size):
                if best is None or value > best[0]:
                    best = (value, size, name)
        if best:
            print(f"{addr:#018x}  {best[2]}+{addr - best[0]:#x}")
        else:
            print(f"{addr:#018x}  <no symbol>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
