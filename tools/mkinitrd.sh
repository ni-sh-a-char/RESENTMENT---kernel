#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Build the initial ramdisk.
#
# USTAR, because it needs no decompressor, no index and no allocation to read,
# which means the kernel can mount it before any driver exists.
set -eu

SRC="${1:-user}"
OUT="${2:-dist/initrd.tar}"

mkdir -p "$(dirname "$OUT")"

# --format=ustar because the kernel parser is a USTAR parser; GNU tar defaults
# to its own extensions, which the kernel would reject.
tar --format=ustar -cf "$OUT" -C "$SRC" .

echo "  initrd: $(wc -c < "$OUT") bytes from $SRC"
