#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Build the kernel with the portable zig+nasm toolchain.
#
# Faster than a full make when iterating, and it reports every broken file
# instead of stopping at the first. Set LINK=0 to compile only.
set -u

ARCH="${ARCH:-x86_64}"
ZIG="${ZIG:-zig}"
NASM="${NASM:-nasm}"
OUT="${OUT:-build/local-$ARCH}"
DIST="${DIST:-dist/$ARCH}"
LINK="${LINK:-1}"

VERSION=0.2.0
CODENAME=kaalachakra
GITREV=$(git rev-parse --short HEAD 2>/dev/null || echo local)

CFLAGS="-target $ARCH-freestanding-none -std=gnu11 -ffreestanding -nostdlib \
-fno-builtin -fno-stack-protector -fno-common -fno-pic -fno-sanitize=undefined \
-Iinclude -Iarch/$ARCH/include -O2 -g \
-DRK_VERSION=\"$VERSION\" -DRK_CODENAME=\"$CODENAME\" -DRK_GITREV=\"$GITREV\" \
-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"

case "$ARCH" in
  x86_64)
	CFLAGS="$CFLAGS -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 -mno-80387 -DRK_ARCH_X86_64=1"
	AI_CFLAGS="-msse -msse2 -mno-red-zone -mcmodel=kernel"
	;;
  aarch64)
	CFLAGS="$CFLAGS -mgeneral-regs-only -DRK_ARCH_AARCH64=1"
	AI_CFLAGS=""
	;;
  riscv64)
	# medany, not medlow: the kernel is linked at 0x80200000, which is just
	# past the 2 GiB that medlow's absolute addressing can reach.
	# zig cc selects the RISC-V feature set from the target triple and
	# rejects -march; a real riscv64-elf-gcc build passes -march=rv64imac.
	CFLAGS="$CFLAGS -mcmodel=medany -DRK_ARCH_RISCV64=1"
	AI_CFLAGS=""
	;;
esac

mkdir -p "$OUT" "$DIST"
fail=0
ok=0
objs=""

# kernel/ai is the one directory allowed to use the vector unit; see fpu.c for
# why the rest of the kernel is compiled without it.
for f in $(find kernel "arch/$ARCH" -name '*.c' | sort); do
	o="$OUT/$(echo "$f" | tr '/' '_').o"
	flags="$CFLAGS"
	case "$f" in
	  kernel/ai/*)
		flags=$(echo "$CFLAGS" | sed 's/-mno-mmx//; s/-mno-sse2//; s/-mno-sse//; s/-mno-80387//; s/-mgeneral-regs-only//; s/-march=rv64imac//; s/-mabi=lp64//')
		flags="$flags $AI_CFLAGS"
		;;
	esac
	if $ZIG cc $flags -c "$f" -o "$o" 2>"$OUT/err.txt"; then
		ok=$((ok + 1))
		objs="$objs $o"
	else
		echo "FAIL $f"
		sed -n '1,25p' "$OUT/err.txt"
		fail=$((fail + 1))
	fi
done

for f in $(find "arch/$ARCH" -name '*.asm' 2>/dev/null | sort); do
	o="$OUT/$(echo "$f" | tr '/' '_').o"
	if $NASM -f elf64 "$f" -o "$o" 2>"$OUT/err.txt"; then
		ok=$((ok + 1))
		objs="$objs $o"
	else
		echo "FAIL $f"
		cat "$OUT/err.txt"
		fail=$((fail + 1))
	fi
done

for f in $(find "arch/$ARCH" -name '*.S' 2>/dev/null | sort); do
	o="$OUT/$(echo "$f" | tr '/' '_').o"
	if $ZIG cc $CFLAGS -c "$f" -o "$o" 2>"$OUT/err.txt"; then
		ok=$((ok + 1))
		objs="$objs $o"
	else
		echo "FAIL $f"
		cat "$OUT/err.txt"
		fail=$((fail + 1))
	fi
done

echo "--- $ARCH: $ok compiled, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
[ "$LINK" = "1" ] || exit 0

$ZIG ld.lld -n -nostdlib -z max-page-size=4096 \
	-T "arch/$ARCH/linker.ld" -o "$DIST/resentment.elf" $objs || exit 1

echo "--- linked $DIST/resentment.elf ($(wc -c < "$DIST/resentment.elf") bytes) ---"
