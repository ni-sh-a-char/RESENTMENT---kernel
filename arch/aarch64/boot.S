/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - aarch64 entry.
 *
 * Entered from the firmware (U-Boot, EDK2, or QEMU loading an ELF directly)
 * with the flattened device tree pointer in x0, at EL2 on most boards and EL1
 * on a few. The job is to reach EL1 with the MMU on and a stack, then call
 * into portable C.
 *
 * Every board differs in what the firmware left behind, so this drops to EL1
 * explicitly rather than assuming, and it zeroes .bss rather than trusting the
 * loader to have done it.
 */

.section .text.boot, "ax"
.global _start

_start:
	/* Only the boot CPU proceeds; the rest park until the SMP code releases
	 * them. MPIDR_EL1 bits 7:0 are the core id within a cluster. */
	mrs     x1, mpidr_el1
	and     x1, x1, #0xFF
	cbz     x1, boot_cpu
park:
	wfe
	b       park

boot_cpu:
	/* Keep the device tree pointer safe across the exception-level change. */
	mov     x19, x0

	/* Which exception level did the firmware leave us at? */
	mrs     x1, CurrentEL
	lsr     x1, x1, #2
	cmp     x1, #2
	b.ne    at_el1

	/* EL2: configure EL1 as 64-bit, disable traps, and eret down. */
	mov     x0, #(1 << 31)          /* HCR_EL2.RW: EL1 is AArch64 */
	msr     hcr_el2, x0

	mov     x0, #0x3c5              /* DAIF masked, EL1h */
	msr     spsr_el2, x0
	adr     x0, at_el1
	msr     elr_el2, x0

	/* A counter that is not running makes every timeout in the kernel wrong,
	 * and some firmware leaves it stopped. */
	mrs     x0, cnthctl_el2
	orr     x0, x0, #3              /* EL1 may read the physical counter */
	msr     cnthctl_el2, x0
	msr     cntvoff_el2, xzr

	eret

at_el1:
	/* Turn off the MMU and caches while we set up; whatever the firmware
	 * configured is not what this kernel expects. */
	mrs     x0, sctlr_el1
	bic     x0, x0, #(1 << 0)       /* M: MMU off */
	bic     x0, x0, #(1 << 2)       /* C: data cache off */
	bic     x0, x0, #(1 << 12)      /* I: instruction cache off */
	msr     sctlr_el1, x0
	isb

	/* Zero .bss. The loader is not required to, and a kernel that boots only
	 * when the memory happened to be zero is a kernel that fails in the
	 * field. */
	adrp    x0, __bss_start
	add     x0, x0, :lo12:__bss_start
	adrp    x1, __bss_end
	add     x1, x1, :lo12:__bss_end
1:
	cmp     x0, x1
	b.hs    2f
	str     xzr, [x0], #8
	b       1b
2:

	adrp    x0, kernel_boot_stack_top
	add     x0, x0, :lo12:kernel_boot_stack_top
	mov     sp, x0

	/* Floating point is used by the AI subsystem, so enable it here rather
	 * than trapping on the first vector instruction. */
	mov     x0, #(3 << 20)          /* CPACR_EL1.FPEN: no trapping */
	msr     cpacr_el1, x0
	isb

	adrp    x0, exception_vectors
	add     x0, x0, :lo12:exception_vectors
	msr     vbar_el1, x0
	isb

	mov     x0, x19                 /* the device tree pointer */
	bl      aarch64_start

hang:
	wfi
	b       hang

/* ------------------------------------------------- secondary processors */

/* Where PSCI CPU_ON lands a core. x0 holds the context id the caller passed,
 * which is the physical address of a two-word record: the stack this core is
 * to use, and its logical number. Everything the boot CPU built - page tables,
 * vectors, .bss - is already in memory, so this only has to point at it.
 *
 * The MMU is off here, exactly as it was for the boot CPU, so every access
 * until mmu_enable_cpu returns is a device access: eight-byte aligned loads
 * only, and no unaligned anything.
 */
.global aarch64_secondary_entry
aarch64_secondary_entry:
	mov     x19, x0

	mrs     x1, CurrentEL
	lsr     x1, x1, #2
	cmp     x1, #2
	b.ne    sec_at_el1

	mov     x0, #(1 << 31)          /* HCR_EL2.RW: EL1 is AArch64 */
	msr     hcr_el2, x0
	mov     x0, #0x3c5              /* DAIF masked, EL1h */
	msr     spsr_el2, x0
	adr     x0, sec_at_el1
	msr     elr_el2, x0
	mrs     x0, cnthctl_el2
	orr     x0, x0, #3
	msr     cnthctl_el2, x0
	msr     cntvoff_el2, xzr
	eret

sec_at_el1:
	mrs     x0, sctlr_el1
	bic     x0, x0, #(1 << 0)
	bic     x0, x0, #(1 << 2)
	bic     x0, x0, #(1 << 12)
	msr     sctlr_el1, x0
	isb

	ldr     x0, [x19]               /* the stack this core was given */
	mov     sp, x0

	mov     x0, #(3 << 20)          /* CPACR_EL1.FPEN: no trapping */
	msr     cpacr_el1, x0
	isb

	adrp    x0, exception_vectors
	add     x0, x0, :lo12:exception_vectors
	msr     vbar_el1, x0
	isb

	mov     x0, x19
	bl      aarch64_secondary_start

sec_hang:
	wfi
	b       sec_hang

/* --------------------------------------------------------------- vectors */

/* The AArch64 vector table is sixteen entries of 128 bytes each, at a 2 KiB
 * aligned address. The alignment is architectural: get it wrong and the CPU
 * takes a different exception than the one that happened. */
.macro VECTOR_ENTRY label
	.align 7
	b       \label
.endm

.align 11
.global exception_vectors
exception_vectors:
	/* Current EL with SP0 */
	VECTOR_ENTRY sync_handler
	VECTOR_ENTRY irq_handler
	VECTOR_ENTRY fiq_handler
	VECTOR_ENTRY serror_handler
	/* Current EL with SPx */
	VECTOR_ENTRY sync_handler
	VECTOR_ENTRY irq_handler
	VECTOR_ENTRY fiq_handler
	VECTOR_ENTRY serror_handler
	/* Lower EL, AArch64 */
	VECTOR_ENTRY sync_handler
	VECTOR_ENTRY irq_handler
	VECTOR_ENTRY fiq_handler
	VECTOR_ENTRY serror_handler
	/* Lower EL, AArch32 */
	VECTOR_ENTRY sync_handler
	VECTOR_ENTRY irq_handler
	VECTOR_ENTRY fiq_handler
	VECTOR_ENTRY serror_handler

/* Save the caller-saved registers a C handler may clobber, plus ELR_EL1 and
 * SPSR_EL1.
 *
 * The two system registers are the subtle part. There is one of each per CPU,
 * so if the handler switches threads - which the timer tick does - the thread
 * that eventually returns here would eret to whatever address the *other*
 * thread was interrupted at. Saving them on this thread's stack is what makes
 * preemption from an exception handler correct rather than intermittently
 * catastrophic. */
.macro SAVE_REGS
	sub     sp, sp, #(16 * 16)
	stp     x0, x1,   [sp, #(16 * 0)]
	stp     x2, x3,   [sp, #(16 * 1)]
	stp     x4, x5,   [sp, #(16 * 2)]
	stp     x6, x7,   [sp, #(16 * 3)]
	stp     x8, x9,   [sp, #(16 * 4)]
	stp     x10, x11, [sp, #(16 * 5)]
	stp     x12, x13, [sp, #(16 * 6)]
	stp     x14, x15, [sp, #(16 * 7)]
	stp     x16, x17, [sp, #(16 * 8)]
	stp     x18, x29, [sp, #(16 * 9)]
	str     x30,      [sp, #(16 * 10)]
	mrs     x0, elr_el1
	mrs     x1, spsr_el1
	stp     x0, x1,   [sp, #(16 * 11)]
.endm

.macro RESTORE_REGS
	ldp     x0, x1,   [sp, #(16 * 11)]
	msr     elr_el1, x0
	msr     spsr_el1, x1
	ldp     x0, x1,   [sp, #(16 * 0)]
	ldp     x2, x3,   [sp, #(16 * 1)]
	ldp     x4, x5,   [sp, #(16 * 2)]
	ldp     x6, x7,   [sp, #(16 * 3)]
	ldp     x8, x9,   [sp, #(16 * 4)]
	ldp     x10, x11, [sp, #(16 * 5)]
	ldp     x12, x13, [sp, #(16 * 6)]
	ldp     x14, x15, [sp, #(16 * 7)]
	ldp     x16, x17, [sp, #(16 * 8)]
	ldp     x18, x29, [sp, #(16 * 9)]
	ldr     x30,      [sp, #(16 * 10)]
	add     sp, sp, #(16 * 16)
.endm

sync_handler:
	SAVE_REGS
	mrs     x0, esr_el1
	mrs     x1, far_el1
	mrs     x2, elr_el1
	/* The saved frame, so a handler that needs to resume somewhere other
	 * than where the fault happened can edit the saved ELR rather than the
	 * register, which RESTORE_REGS would overwrite. */
	mov     x3, sp
	bl      aarch64_sync_exception
	RESTORE_REGS
	eret

irq_handler:
	SAVE_REGS
	bl      aarch64_irq
	RESTORE_REGS
	eret

fiq_handler:
	SAVE_REGS
	bl      aarch64_irq
	RESTORE_REGS
	eret

serror_handler:
	SAVE_REGS
	mrs     x0, esr_el1
	bl      aarch64_serror
	RESTORE_REGS
	eret

/* ------------------------------------------------------- context switch */

/* void arch_context_switch(void **save_sp, void *load_sp)
 * Only the callee-saved registers cross a switch; the ABI already treats the
 * rest as dead. */
.global arch_context_switch
arch_context_switch:
	sub     sp, sp, #(16 * 6)
	stp     x19, x20, [sp, #(16 * 0)]
	stp     x21, x22, [sp, #(16 * 1)]
	stp     x23, x24, [sp, #(16 * 2)]
	stp     x25, x26, [sp, #(16 * 3)]
	stp     x27, x28, [sp, #(16 * 4)]
	stp     x29, x30, [sp, #(16 * 5)]

	mov     x2, sp
	str     x2, [x0]
	mov     sp, x1

	ldp     x19, x20, [sp, #(16 * 0)]
	ldp     x21, x22, [sp, #(16 * 1)]
	ldp     x23, x24, [sp, #(16 * 2)]
	ldp     x25, x26, [sp, #(16 * 3)]
	ldp     x27, x28, [sp, #(16 * 4)]
	ldp     x29, x30, [sp, #(16 * 5)]
	add     sp, sp, #(16 * 6)
	ret

/* A new thread arrives here with its entry point in x19 and argument in x20,
 * placed there by arch_thread_init. */
.global aarch64_thread_trampoline
aarch64_thread_trampoline:
	mov     x0, x19
	mov     x1, x20
	mov     x29, xzr
	bl      aarch64_thread_entry
	brk     #0

.section .bss
.align 16
.global kernel_boot_stack_bottom
kernel_boot_stack_bottom:
	.space 65536
.global kernel_boot_stack_top
kernel_boot_stack_top:
