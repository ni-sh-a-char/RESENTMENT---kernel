# SPDX-License-Identifier: Apache-2.0
# RESENTMENT kernel - build system.
#
#   make                     build for x86_64
#   make ARCH=aarch64        build for ARM64
#   make ARCH=riscv64        build for RISC-V
#   make all-arch            build all three, which is the portability check
#   make iso                 GRUB-bootable ISO (x86_64)
#   make run                 boot it under QEMU
#   make test                run the host test suite
#   make verify              check that the image is actually bootable
#   make TOOLCHAIN=gcc       use a real <arch>-elf-gcc cross toolchain
#
# Two toolchains are supported on purpose. `zig` is one download and
# cross-compiles to all three architectures, which is what makes this tree
# buildable on a laptop with no cross-gcc; `gcc` is what the Docker image and
# CI use. They produce equivalent ELFs.

ARCH        ?= x86_64
BUILD       ?= build/$(ARCH)
DIST        ?= dist/$(ARCH)
TOOLCHAIN   ?= zig
V           ?= 0

VERSION     := 2.0.0
CODENAME    := kaalachakra
GIT_REV     := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

ifeq ($(V),1)
Q :=
else
Q := @
endif

# ---------------------------------------------------------------- toolchain

ZIG        ?= zig
NASM       ?= nasm
QEMU_EXTRA ?=

# Prefer a bare-metal <arch>-elf- toolchain, fall back to the Linux-targeting
# cross compiler every distribution actually packages. With -ffreestanding and
# -nostdlib the second produces the same objects and is far easier to install.
CROSS ?= $(shell if command -v $(ARCH)-elf-gcc >/dev/null 2>&1; then \
                     echo $(ARCH)-elf-; \
                 elif command -v $(ARCH)-linux-gnu-gcc >/dev/null 2>&1; then \
                     echo $(ARCH)-linux-gnu-; \
                 else echo $(ARCH)-elf-; fi)

ifeq ($(TOOLCHAIN),zig)
  CC       := $(ZIG) cc -target $(ARCH)-freestanding-none
  LD       := $(ZIG) ld.lld
  HOSTCC   := $(ZIG) cc
  # Zig turns UBSan on by default; a freestanding kernel has no runtime for it.
  EXTRA_CFLAGS := -fno-sanitize=undefined
else
  CC       := $(CROSS)gcc
  LD       := $(CROSS)ld
  HOSTCC   := cc
  EXTRA_CFLAGS :=
endif

# ------------------------------------------------------------- arch settings

ifeq ($(ARCH),x86_64)
  ARCH_CFLAGS := -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 -mno-80387
  # kernel/ai is the one place float is legal; see kernel/core/fpu.c.
  AI_CFLAGS   := -mno-red-zone -mcmodel=kernel -msse -msse2
  QEMU        := qemu-system-x86_64
  QEMUFLAGS   := -cdrom $(DIST)/resentment.iso -serial stdio -m 512M -smp 2 \
                 -no-reboot -display none
endif
ifeq ($(ARCH),aarch64)
  ARCH_CFLAGS := -mgeneral-regs-only
  AI_CFLAGS   :=
  QEMU        := qemu-system-aarch64
  QEMUFLAGS   := -M virt -cpu cortex-a72 -m 512M -nographic \
                 -kernel $(DIST)/resentment.elf
endif
ifeq ($(ARCH),riscv64)
  # medany, not medlow: the kernel is linked at 0x80200000, past the 2 GiB
  # that medlow's absolute addressing can reach.
  ifeq ($(TOOLCHAIN),zig)
    ARCH_CFLAGS := -mcmodel=medany
  else
    ARCH_CFLAGS := -mcmodel=medany -march=rv64imac -mabi=lp64
  endif
  AI_CFLAGS   := $(ARCH_CFLAGS)
  QEMU        := qemu-system-riscv64
  QEMUFLAGS   := -M virt -m 512M -nographic -kernel $(DIST)/resentment.elf
endif

ARCH_UPPER := $(shell echo $(ARCH) | tr a-z A-Z)

# ------------------------------------------------------------------- flags

WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wundef \
            -Wno-missing-field-initializers -Wvla

BASE_CFLAGS := -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
          -fno-stack-protector -fno-omit-frame-pointer -fno-common \
          -fno-pic -fno-pie \
          -Iinclude -Iarch/$(ARCH)/include \
          -DRK_ARCH_$(ARCH_UPPER)=1 \
          -DRK_VERSION=\"$(VERSION)\" -DRK_CODENAME=\"$(CODENAME)\" \
          -DRK_GITREV=\"$(GIT_REV)\" \
          -O2 -g $(WARNINGS) $(EXTRA_CFLAGS)

CFLAGS    := $(BASE_CFLAGS) $(ARCH_CFLAGS)
CFLAGS_AI := $(BASE_CFLAGS) $(AI_CFLAGS)

LDFLAGS := -n -nostdlib -z max-page-size=4096

# ------------------------------------------------------------------ sources

C_SRCS  := $(shell find kernel -name '*.c' 2>/dev/null) \
           $(shell find arch/$(ARCH) -name '*.c' 2>/dev/null)
ASM_SRCS:= $(shell find arch/$(ARCH) -name '*.asm' 2>/dev/null)
S_SRCS  := $(shell find arch/$(ARCH) -name '*.S' 2>/dev/null)

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) \
        $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS)) \
        $(patsubst %.S,$(BUILD)/%.o,$(S_SRCS))

DEPS := $(OBJS:.o=.d)

LINKER_SCRIPT := arch/$(ARCH)/linker.ld
KERNEL_ELF    := $(DIST)/resentment.elf

# ------------------------------------------------------------------ targets

.PHONY: all kernel iso run run-iso run-script debug test verify check clean qemu-test-all site site-serve \
        distclean help info all-arch initrd toolchain qemu-test kaalka-check

all: kernel

kernel: $(KERNEL_ELF)

$(KERNEL_ELF): $(OBJS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	$(Q)$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJS)
	@echo "  SIZE    $$(wc -c < $@) bytes"

# The AI subsystem is compiled with the vector unit enabled and is the only
# directory allowed to use floating point. This rule is what enforces that.
$(BUILD)/kernel/ai/%.o: kernel/ai/%.c
	@mkdir -p $(dir $@)
	@echo "  CC/simd $<"
	$(Q)$(CC) $(CFLAGS_AI) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(Q)$(NASM) -f elf64 $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# Building every architecture is the portability check, and it is cheap.
# $(MAKE) is quoted because on Windows it expands to a path containing spaces
# and parentheses, which the shell then tries to parse.
all-arch:
	$(Q)"$(MAKE)" ARCH=x86_64  kernel
	$(Q)"$(MAKE)" ARCH=aarch64 kernel
	$(Q)"$(MAKE)" ARCH=riscv64 kernel
	@echo "all three architectures built"

# --------------------------------------------------------------------- iso

iso: $(DIST)/resentment.iso

$(DIST)/resentment.iso: $(KERNEL_ELF) arch/x86_64/grub.cfg $(DIST)/initrd.tar
	@mkdir -p $(BUILD)/isoroot/boot/grub
	$(Q)cp $(KERNEL_ELF) $(BUILD)/isoroot/boot/resentment.elf
	$(Q)cp arch/x86_64/grub.cfg $(BUILD)/isoroot/boot/grub/grub.cfg
	$(Q)cp $(DIST)/initrd.tar $(BUILD)/isoroot/boot/initrd.tar
	@echo "  ISO     $@"
	$(Q)grub-mkrescue -o $@ $(BUILD)/isoroot 2>/dev/null

initrd: $(DIST)/initrd.tar

$(DIST)/initrd.tar: $(shell find user -type f 2>/dev/null)
	@mkdir -p $(dir $@)
	@echo "  INITRD  $@"
	$(Q)sh tools/mkinitrd.sh user $@

# ------------------------------------------------------------ direct boot

# QEMU's own -kernel loader implements Multiboot 1 and refuses a 64-bit ELF, so
# this repackages the same segments at the same physical addresses in 32-bit
# headers. It is what makes `make run` work with no ISO, no bootloader and no
# disk image - which is the difference between a one-command test and a
# five-minute one.
MB1_ELF := $(DIST)/resentment32.elf

$(MB1_ELF): $(KERNEL_ELF)
	$(Q)$(PYTHON) tools/mkmb1.py $< $@

# Part of the default build, not an extra step. A stale repackaged image that
# silently boots yesterday's kernel is the kind of thing that costs an
# afternoon, and there is no reason for the two to ever be out of step.
ifeq ($(ARCH),x86_64)
kernel: $(MB1_ELF)
endif

# Probe by running it, not by looking it up: Windows ships a `python3` shim
# that exists on PATH and fails when executed.
PYTHON ?= $(shell if python3 -c "" >/dev/null 2>&1; then echo python3; else echo python; fi)

# --------------------------------------------------------------------- run

ifeq ($(ARCH),x86_64)
run: $(MB1_ELF) $(DIST)/initrd.tar
	$(QEMU) -kernel $(MB1_ELF) -initrd $(DIST)/initrd.tar -m 512M \
	        -nographic -no-reboot $(QEMU_EXTRA)
else
run: kernel
	$(QEMU) $(QEMUFLAGS) $(QEMU_EXTRA)
endif

run-iso: iso
	$(QEMU) $(QEMUFLAGS) $(QEMU_EXTRA)

# Boot, run one SHE script, and power off. This is how a script drives the
# kernel with no console at all.
run-script: $(MB1_ELF) $(DIST)/initrd.tar
	$(QEMU) -kernel $(MB1_ELF) -initrd $(DIST)/initrd.tar -m 512M \
	        -nographic -no-reboot \
	        -append "run=$(SCRIPT) once allow=all"

debug: $(MB1_ELF)
	$(QEMU) -kernel $(MB1_ELF) -m 512M -nographic -no-reboot -s -S

# The integration test: boot, drive the shell over a serial link, check the
# replies. Everything from the interrupt controller to the SHE compiler has to
# work for a single line to come back correct.
qemu-test: $(MB1_ELF) $(DIST)/initrd.tar
	$(Q)$(PYTHON) tools/qemu-expect.py --qemu $(QEMU) \
	    --kernel $(MB1_ELF) --initrd $(DIST)/initrd.tar

# Every architecture, single core and on four. Needs all three images built,
# which is what all-arch is for.
qemu-test-all: all-arch $(MB1_ELF) $(DIST)/initrd.tar
	$(Q)$(PYTHON) tools/qemu-expect.py --all

# ------------------------------------------------------------------- site

# The documentation site is generated from the Markdown in this tree, so the
# published docs and the docs a contributor reads cannot drift apart.
site:
	$(Q)$(PYTHON) tools/mksite.py
	$(Q)$(PYTHON) tools/checklinks.py site

site-serve:
	$(Q)$(PYTHON) tools/mksite.py --serve

# -------------------------------------------------------------------- tests

# The portable half of the kernel is compiled for the host and exercised with
# real assertions. This is what makes the tree testable without hardware.
test:
	$(Q)HOSTCC="$(HOSTCC)" sh tools/runtests.sh

verify: kernel
	$(Q)$(PYTHON) tools/verify-image.py $(KERNEL_ELF)

kaalka-check: test
	$(Q)$(PYTHON) tools/kaalka_ref.py

check: test verify kaalka-check
	@echo
	@echo "host tests, image verification and the Kaalka cross-check all passed"
	@echo "run 'make qemu-test' as well if QEMU is installed"

# ------------------------------------------------------------------- misc

info:
	@echo "RESENTMENT $(VERSION) ($(CODENAME)) rev $(GIT_REV)"
	@echo "arch      : $(ARCH)"
	@echo "toolchain : $(TOOLCHAIN)  [$(CC)]"
	@echo "sources   : $(words $(C_SRCS)) C, $(words $(ASM_SRCS)) nasm, $(words $(S_SRCS)) gas"
	@echo "output    : $(KERNEL_ELF)"

toolchain:
	$(Q)sh tools/get-toolchain.sh

clean:
	$(Q)rm -rf build dist
	@echo "  CLEAN"

distclean: clean
	$(Q)rm -rf .toolchain

help:
	@echo "RESENTMENT - a capability-secure, AI-native kernel"
	@echo ""
	@echo "  make [ARCH=x86_64|aarch64|riscv64]   build the kernel"
	@echo "  make all-arch                        build all three"
	@echo "  make run                             boot under QEMU, no ISO needed"
	@echo "  make iso / make run-iso              bootable GRUB ISO (x86_64)"
	@echo "  make run-script SCRIPT=/boot/bin/attest.she"
	@echo "                                       boot, run one script, power off"
	@echo "  make test                            host test suite (1440 assertions)"
	@echo "  make qemu-test                       boot and drive the shell"
	@echo "  make check                           tests, image check, Kaalka cross-check"
	@echo "  make verify                          check the image is bootable"
	@echo "  make info                            show the build configuration"
	@echo "  make toolchain                       fetch a portable zig and nasm"
	@echo "  make clean"
