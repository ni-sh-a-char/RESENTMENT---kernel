#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Fetch a portable zig and nasm into .toolchain/.
#
# Two downloads and no installer: this is what lets someone clone the tree and
# build all three architectures without a cross-compiler on their machine.
set -eu

ZIG_VERSION=0.15.2
NASM_VERSION=2.16.03
DEST=.toolchain

case "$(uname -s)" in
  Linux)  ZIG_OS=linux;   ZIG_EXT=tar.xz ;;
  Darwin) ZIG_OS=macos;   ZIG_EXT=tar.xz ;;
  MINGW*|MSYS*|CYGWIN*) ZIG_OS=windows; ZIG_EXT=zip ;;
  *) echo "unknown platform: $(uname -s)"; exit 1 ;;
esac

case "$(uname -m)" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  *) echo "unknown machine: $(uname -m)"; exit 1 ;;
esac

mkdir -p "$DEST"
cd "$DEST"

if [ ! -d "zig-$ARCH-$ZIG_OS-$ZIG_VERSION" ]; then
	URL="https://ziglang.org/download/$ZIG_VERSION/zig-$ARCH-$ZIG_OS-$ZIG_VERSION.$ZIG_EXT"
	echo "fetching $URL"
	curl -fL -o "zig.$ZIG_EXT" "$URL"
	case "$ZIG_EXT" in
	  zip) unzip -q "zig.$ZIG_EXT" ;;
	  *)   tar xf "zig.$ZIG_EXT" ;;
	esac
	rm -f "zig.$ZIG_EXT"
fi

if [ ! -d "nasm-$NASM_VERSION" ]; then
	case "$ZIG_OS" in
	  windows)
		curl -fL -o nasm.zip \
		  "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VERSION/win64/nasm-$NASM_VERSION-win64.zip"
		unzip -q nasm.zip
		rm -f nasm.zip
		;;
	  *)
		echo "install nasm from your package manager (only x86_64 needs it)"
		;;
	esac
fi

cd ..
echo
echo "toolchain ready. Build with:"
echo "  make ZIG=$PWD/$DEST/zig-$ARCH-$ZIG_OS-$ZIG_VERSION/zig \\"
echo "       NASM=$PWD/$DEST/nasm-$NASM_VERSION/nasm"
