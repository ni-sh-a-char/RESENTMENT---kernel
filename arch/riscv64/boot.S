/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - riscv64 entry.
 *
 * Entered from OpenSBI in supervisor mode with the hart id in a0 and the
 * device tree pointer in a1. RISC-V firmware does more for us than ARM
 * firmware does: the machine-mode setup is already done, so this only has to
 * pick a boot hart, set up a stack, zero .bss and install the trap vector.
 */

.section .text.boot, "ax"
.global _start

_start:
	/* gp comes first, and nothing may be put in front of it.
	 *
	 * A RISC-V linker relaxes `la rd, sym` into `addi rd, gp, offset` for any
	 * symbol near __global_pointer$, so an instruction that mentions a symbol
	 * before gp holds a sane value computes an address from garbage. GNU ld
	 * does this relaxation and lld does not, which makes the bug invisible
	 * under half the toolchains that build this tree - it presents as a kernel
	 * that produces no output whatsoever.
	 *
	 * The norelax pragma around the load of gp itself is the same problem one
	 * step further in: relaxing that would make it depend on its own result. */
.option push
.option norelax
	la      gp, __global_pointer$
.option pop

	/* Which hart runs the kernel is the firmware's choice, not ours: OpenSBI
	 * picks one and it is regularly not hart zero. So the first hart to
	 * arrive claims the boot role and the rest park - on a system whose
	 * firmware releases every hart at once, exactly one wins the swap. */
	la      t0, boot_hart_claimed
	li      t1, 1
	amoswap.w t2, t1, (t0)
	bnez    t2, park

	/* Remembered for the SMP code, which must not try to start the hart it
	 * is already running on. */
	la      t0, boot_hart_id
	sd      a0, 0(t0)

	la      sp, kernel_boot_stack_top

	/* Logical core zero. tp is the per-core number for the rest of the
	 * kernel's life; nothing else in this build uses it, and the context
	 * switch deliberately does not save it, because it belongs to the core
	 * rather than to the thread running on it. */
	mv      tp, zero

	/* Zero .bss. OpenSBI does not, and a kernel that only boots when memory
	 * happened to be zero is a kernel that fails in the field. */
	la      t0, __bss_start
	la      t1, __bss_end
1:
	bgeu    t0, t1, 2f
	sd      zero, 0(t0)
	addi    t0, t0, 8
	j       1b
2:

	la      t0, trap_vector
	csrw    stvec, t0

	/* Enable the supervisor interrupts the kernel handles: timer, software
	 * and external. Leave the global enable off until arch_init says so. */
	li      t0, (1 << 1) | (1 << 5) | (1 << 9)
	csrw    sie, t0
	csrw    sip, zero

	mv      a0, a1                  /* the device tree pointer */
	call    riscv64_start

park:
	wfi
	j       park

/* ------------------------------------------------- secondary processors */

/* Where SBI HSM hart_start lands a core: supervisor mode, a0 = hart id,
 * a1 = the opaque value the caller passed, which here is the address of a
 * two-word record holding the stack and the logical core number. Address
 * translation is off on this port, so there is nothing else to set up. */
.global riscv64_secondary_entry
riscv64_secondary_entry:
.option push
.option norelax
	la      gp, __global_pointer$
.option pop

	ld      tp, 8(a1)
	ld      sp, 0(a1)

	la      t0, trap_vector
	csrw    stvec, t0

	/* Timer and software interrupts only. External interrupts are routed to
	 * the boot hart's PLIC context, and claiming them from another hart
	 * would acknowledge an interrupt this core was never sent. */
	li      t0, (1 << 1) | (1 << 5)
	csrw    sie, t0
	csrw    sip, zero

	mv      a0, tp
	call    riscv64_secondary_start
1:
	wfi
	j       1b

/* ----------------------------------------------------------------- traps */

/* stvec must be four-byte aligned, and in direct mode the low two bits are the
 * mode field rather than address bits. */
.align 4
.global trap_vector
trap_vector:
	addi    sp, sp, -256
	sd      ra,   0(sp)
	sd      t0,   8(sp)
	sd      t1,  16(sp)
	sd      t2,  24(sp)
	sd      t3,  32(sp)
	sd      t4,  40(sp)
	sd      t5,  48(sp)
	sd      t6,  56(sp)
	sd      a0,  64(sp)
	sd      a1,  72(sp)
	sd      a2,  80(sp)
	sd      a3,  88(sp)
	sd      a4,  96(sp)
	sd      a5, 104(sp)
	sd      a6, 112(sp)
	sd      a7, 120(sp)

	/* sepc and sstatus are CSRs, not registers: there is one of each for the
	 * whole hart. If the handler switches threads - which the timer tick
	 * does - the thread that eventually returns here would sret to whatever
	 * pc the *other* thread trapped at. Saving them on this thread's stack is
	 * what makes preemption from a trap handler correct rather than
	 * intermittently catastrophic. */
	csrr    t0, sepc
	sd      t0, 128(sp)
	csrr    t0, sstatus
	sd      t0, 136(sp)

	csrr    a0, scause
	csrr    a1, sepc
	csrr    a2, stval
	call    riscv64_trap

	ld      t0, 136(sp)
	csrw    sstatus, t0
	ld      t0, 128(sp)
	csrw    sepc, t0

	ld      ra,   0(sp)
	ld      t0,   8(sp)
	ld      t1,  16(sp)
	ld      t2,  24(sp)
	ld      t3,  32(sp)
	ld      t4,  40(sp)
	ld      t5,  48(sp)
	ld      t6,  56(sp)
	ld      a0,  64(sp)
	ld      a1,  72(sp)
	ld      a2,  80(sp)
	ld      a3,  88(sp)
	ld      a4,  96(sp)
	ld      a5, 104(sp)
	ld      a6, 112(sp)
	ld      a7, 120(sp)
	addi    sp, sp, 256
	sret

/* ------------------------------------------------------- context switch */

/* void arch_context_switch(void **save_sp, void *load_sp)
 * The RISC-V ABI makes s0 through s11 callee-saved; everything else is
 * already dead across a call. */
.global arch_context_switch
arch_context_switch:
	addi    sp, sp, -112
	sd      ra,  0(sp)
	sd      s0,  8(sp)
	sd      s1, 16(sp)
	sd      s2, 24(sp)
	sd      s3, 32(sp)
	sd      s4, 40(sp)
	sd      s5, 48(sp)
	sd      s6, 56(sp)
	sd      s7, 64(sp)
	sd      s8, 72(sp)
	sd      s9, 80(sp)
	sd      s10, 88(sp)
	sd      s11, 96(sp)

	sd      sp, 0(a0)
	mv      sp, a1

	ld      ra,  0(sp)
	ld      s0,  8(sp)
	ld      s1, 16(sp)
	ld      s2, 24(sp)
	ld      s3, 32(sp)
	ld      s4, 40(sp)
	ld      s5, 48(sp)
	ld      s6, 56(sp)
	ld      s7, 64(sp)
	ld      s8, 72(sp)
	ld      s9, 80(sp)
	ld      s10, 88(sp)
	ld      s11, 96(sp)
	addi    sp, sp, 112
	ret

/* A new thread arrives with its entry point in s1 and argument in s2. */
.global riscv64_thread_trampoline
riscv64_thread_trampoline:
	mv      a0, s1
	mv      a1, s2
	call    riscv64_thread_entry
	unimp

/* In .data, not .bss: these are written before .bss is zeroed. */
.section .data
.align 8
.global boot_hart_id
boot_hart_id:
	.dword 0
boot_hart_claimed:
	.word 0

.section .bss
.align 16
.global kernel_boot_stack_bottom
kernel_boot_stack_bottom:
	.space 65536
.global kernel_boot_stack_top
kernel_boot_stack_top:
