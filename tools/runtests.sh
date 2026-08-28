#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Build and run the host test suite.
#
# The portable half of the kernel is compiled for the host and exercised with
# real assertions. It is the same source the kernel ships, not a mock: the
# shim in tests/host supplies a synthetic machine underneath it.
set -eu

HOSTCC="${HOSTCC:-cc}"
OUT="${OUT:-build/hosttests}"

SOURCES="
tests/host/shim.c
tests/host/main.c
kernel/lib/string.c
kernel/lib/printf.c
kernel/lib/errno.c
kernel/lib/int128.c
kernel/lib/calendar.c
kernel/mm/pmm.c
kernel/mm/kheap.c
kernel/crypto/sha256.c
kernel/crypto/chacha20.c
kernel/crypto/util.c
kernel/crypto/kaalka.c
kernel/graph/graph.c
kernel/graph/memfab.c
kernel/she/value.c
kernel/she/compile.c
kernel/she/vm.c
kernel/she/stdlib.c
tests/host/stubs.c
"

CFLAGS="-std=gnu11 -DRK_HOSTED=1 -Iinclude -Ikernel/she -O1 -g \
-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
-Wno-unused-function -fno-strict-aliasing"

# Windows will not launch an extensionless PE, and the cross-check tool needs
# to be able to run this binary too.
case "$(uname -s 2>/dev/null || echo unknown)" in
  MINGW*|MSYS*|CYGWIN*|Windows*) EXE=".exe" ;;
  *) EXE="" ;;
esac

mkdir -p "$OUT"
echo "building host tests with $HOSTCC"
$HOSTCC $CFLAGS $SOURCES -o "$OUT/hosttests$EXE" -lm

echo
"$OUT/hosttests$EXE" "$@"
