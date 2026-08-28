# SPDX-License-Identifier: Apache-2.0
# RESENTMENT build environment.
#
# Everything needed to build all three architectures, run the host test suite
# and boot the result under QEMU. The image is pinned to a Debian release so a
# build from a year ago still reproduces.
#
#   docker build -t resentment-build buildenv
#   docker run --rm -v "$PWD":/src -w /src resentment-build make check
#   docker run --rm -v "$PWD":/src -w /src resentment-build make iso

FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc-x86-64-linux-gnu \
        gcc-aarch64-linux-gnu \
        gcc-riscv64-linux-gnu \
        nasm \
        make \
        git \
        python3 \
        xorriso \
        mtools \
        grub-pc-bin \
        grub-efi-amd64-bin \
        grub-common \
        qemu-system-x86 \
        qemu-system-arm \
        qemu-system-misc \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

# The Debian cross compilers are named for Linux targets, but with
# -ffreestanding -nostdlib they produce exactly the bare-metal objects this
# kernel needs, and they are far easier to install than a hand-built
# <arch>-elf- toolchain.
ENV CROSS_X86_64=x86_64-linux-gnu-
ENV CROSS_AARCH64=aarch64-linux-gnu-
ENV CROSS_RISCV64=riscv64-linux-gnu-

WORKDIR /src

CMD ["make", "help"]
