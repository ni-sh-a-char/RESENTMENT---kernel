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
# Zig is preferred because `make toolchain` fetches it and it targets all three
# architectures from one binary. But it is not required: if it is not on PATH,
# fall back to the system compiler rather than failing with `zig: not found`,
# which is what a CI runner and a distribution package both give you.
TOOLCHAIN   ?= $(shell command -v $(ZIG) >/dev/null 2>&1 && echo zig || echo gcc)
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
  ifneq ($(TOOLCHAIN),zig)
    # GCC's outline atomics call into libgcc (__aarch64_ldadd4_acq_rel and
    # friends) so that one binary can use LSE where the CPU has it. A
    # freestanding kernel does not link libgcc, so every atomic becomes an
    # undefined symbol at link time. Inline them instead - in both flag sets,
    # because kernel/ai is compiled with its own and uses atomics too.
    ARCH_CFLAGS += -mno-outline-atomics
    AI_CFLAGS   += -mno-outline-atomics
  endif
  QEMU        := qemu-system-aarch64
  QEMUFLAGS   := -M virt -cpu cortex-a72 -m 512M -nographic \
                 -kernel $(DIST)/resentment.elf
endif
ifeq ($(ARCH),riscv64)
  # medany, not medlow: the kernel is linked at 0x80200000, past the 2 GiB
  # that medlow's absolute addressing can reach.
  #
  # zig cc selects the RISC-V feature set from the target triple and rejects
  # -march. GCC needs it spelled out, and needs zicsr and zifencei named
  # explicitly: binutils 2.38 split the CSR and fence.i instructions out of the
  # base ISA, so a plain rv64imac assembles every csrr and csrw in this port
  # into "unrecognized opcode".
  ifeq ($(TOOLCHAIN),zig)
    ARCH_CFLAGS := -mcmodel=medany
    AI_CFLAGS   := $(ARCH_CFLAGS)
  else
    ARCH_CFLAGS := -mcmodel=medany -march=rv64imac_zicsr_zifencei -mabi=lp64
    # kernel/ai is the one directory allowed to touch the float unit, so it is
    # the one place F and D belong in -march. Without them GCC compiles every
    # float into a libgcc soft-float call - __addsf3 and its family - which a
    # freestanding kernel does not link. The ABI stays lp64 either way:
    # arguments travel in integer registers, and the float unit is used only
    # inside the rk_fpu_begin/rk_fpu_end window.
    AI_CFLAGS   := -mcmodel=medany -march=rv64imafdc_zicsr_zifencei -mabi=lp64
  endif
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

# The initial ramdisk, linked into the image. A bootloader module is still
# preferred and still wins at run time, but QEMU hands no module to a bare ELF
# on the ARM and RISC-V virt machines - so without this, two of the three
# architectures have no /boot and the integration suite quietly tests less.
GEN_INITRD_C := $(BUILD)/gen/initrd_blob.c
GEN_INITRD_O := $(BUILD)/gen/initrd_blob.o

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) \
        $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS)) \
        $(patsubst %.S,$(BUILD)/%.o,$(S_SRCS)) \
        $(GEN_INITRD_O)

DEPS := $(OBJS:.o=.d)

# -MF and -MT are spelled out rather than relying on -MMD alone. `zig cc`
# accepts -MMD and then writes no dependency file at all, and when told where
# to write one it names the target after its own temporary object. Either
# failure is silent: the build keeps working, and editing a header stops
# rebuilding anything that includes it.
DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@

LINKER_SCRIPT := arch/$(ARCH)/linker.ld
KERNEL_ELF    := $(DIST)/resentment.elf

# ------------------------------------------------------------------- user

# Programs that run in ring 3. They are built with the same compiler as the
# kernel and linked against nothing at all: user/src/rksys.h is the whole of
# their dependency on the system, and it is three inline assembly stubs.
#
# Flags differ from the kernel's in exactly one way that matters: no
# -mcmodel=kernel and no -mno-red-zone, because a user program is neither in
# the top two gigabytes nor forbidden the red zone.
USER_SRCS := $(wildcard user/src/*.c)
USER_ELF  := $(BUILD)/user/init
USER_LD   := $(BUILD)/user/user.ld

USER_CFLAGS := -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
               -fno-stack-protector -fno-common -fno-pic -fno-pie \
               -Os -Wall -Wextra -Wno-unused-parameter $(EXTRA_CFLAGS)

# Where the program is linked. x86_64 uses the traditional low address; aarch64
# must sit above the kernel's identity map, which occupies the bottom eight
# gigabytes of every address space on this port. See RK_USER_VA_MIN.
USER_BASE := 0x400000
ifeq ($(ARCH),riscv64)
  ifneq ($(TOOLCHAIN),zig)
    USER_CFLAGS += -march=rv64imac_zicsr_zifencei -mabi=lp64
  endif
  USER_CFLAGS += -mcmodel=medany
endif
ifeq ($(ARCH),aarch64)
  USER_BASE := 0x200000000
  USER_CFLAGS += -mcmodel=large
  ifneq ($(TOOLCHAIN),zig)
    USER_CFLAGS += -mno-outline-atomics
  endif
endif

# The base address is substituted into the script rather than passed with
# --defsym: a symbol defined on the linker command line is not visible to the
# script's own DEFINED() test, so the load address silently stays at its
# default and the kernel then rejects the image for being outside the user
# range - which reads as a broken loader rather than a broken link.
$(USER_LD): user/src/user.ld.in
	@mkdir -p $(dir $@)
	$(Q)sed 's/@USER_BASE@/$(USER_BASE)/' $< > $@

$(USER_ELF): $(USER_SRCS) $(USER_LD) user/src/rksys.h
	@mkdir -p $(dir $@)
	@echo "  CC/user $(USER_SRCS)"
	$(Q)$(CC) $(USER_CFLAGS) -Wl,-T,$(USER_LD) -Wl,--build-id=none \
	    -o $@ $(USER_SRCS)
	@echo "  USER    $@ ($$(wc -c < $@) bytes)" 

# ------------------------------------------------------------------ targets

.PHONY: all kernel iso run run-iso run-script debug test verify check clean qemu-test-all site site-serve \
        distclean help info all-arch initrd toolchain qemu-test kaalka-check

# Named explicitly rather than left to "whichever target appears first",
# because that is a property of file order and quietly changes when a rule is
# added above this line - which presents as a build that succeeds and produces
# nothing.
.DEFAULT_GOAL := all

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
	$(Q)$(CC) $(CFLAGS_AI) $(DEPFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(Q)$(NASM) -f elf64 $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(Q)$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

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

$(GEN_INITRD_C): $(DIST)/initrd.tar
	@mkdir -p $(dir $@)
	$(Q)$(PYTHON) tools/bin2c.py $< $@

$(GEN_INITRD_O): $(GEN_INITRD_C)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The ramdisk is staged rather than packed straight out of user/, because it
# now carries a compiled program as well as scripts, and that program is
# architecture specific. user/src is the source of it and does not belong in
# the image.
$(DIST)/initrd.tar: $(shell find user -type f 2>/dev/null) $(USER_ELF)
	@mkdir -p $(dir $@)
	@rm -rf $(BUILD)/initrd-root
	@mkdir -p $(BUILD)/initrd-root/bin
	$(Q)cp -r user/bin user/etc $(BUILD)/initrd-root/
	$(Q)cp $(USER_ELF) $(BUILD)/initrd-root/bin/init
	@echo "  INITRD  $@"
	$(Q)sh tools/mkinitrd.sh $(BUILD)/initrd-root $@

# ------------------------------------------------------------ direct boot

# QEMU's own -kernel loader implements Multiboot 1 and refuses a 64-bit ELF, so
# this repackages the same segments at the same physical addresses in 32-bit
# headers. It is what makes `make run` work with no ISO, no bootloader and no
# disk image - which is the difference between a one-command test and a
# five-minute one.
MB1_ELF := $(DIST)/resentment32.elf

$(MB1_ELF): $(KERNEL_ELF)
	$(Q)$(PYTHON) tools/mkmb1.py $< $@

# Both are part of the default build, not extra steps. A stale repackaged image
# that silently boots yesterday's kernel is the kind of thing that costs an
# afternoon; a missing initrd is worse, because the integration harness skips
# the checks that need one and still reports success.
ifeq ($(ARCH),x86_64)
kernel: $(MB1_ELF) $(DIST)/initrd.tar
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
