/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - x86_64 system call plumbing.
 *
 * SYSCALL/SYSRET rather than an interrupt gate: the interrupt path costs
 * several hundred cycles of descriptor loading that the fast path does not
 * need. The legacy int 0x80 gate is kept because it works before the MSRs are
 * programmed, which makes early bring-up debuggable.
 */
#include <arch/x86.h>
#include <rk/syscall.h>
#include <rk/log.h>
#include <rk/errno.h>

#undef RK_SUBSYS
#define RK_SUBSYS "syscall"

extern void x86_syscall_entry(void);

/* Called from syscall_entry.asm with the argument block on the stack. */
s64 x86_syscall_handler(struct rk_syscall_args *a);

s64 x86_syscall_handler(struct rk_syscall_args *a)
{
	return rk_syscall_dispatch(a);
}

/* The trap-gate path, for int 0x80. Same ABI as the fast path so the
 * dispatcher does not care which arrived. */
s64 x86_syscall_from_trap(struct trapframe *f);

s64 x86_syscall_from_trap(struct trapframe *f)
{
	struct rk_syscall_args a = {
		.nr = f->rax,
		.a0 = f->rdi, .a1 = f->rsi, .a2 = f->rdx,
		.a3 = f->r10, .a4 = f->r8,  .a5 = f->r9,
	};
	return rk_syscall_dispatch(&a);
}

void x86_syscall_init(void)
{
	u32 a, b, c, d;
	cpuid_raw(0x80000001, 0, &a, &b, &c, &d);
	if (!(d & (1u << 11))) {
		pr_warn("this CPU has no SYSCALL instruction; using int 0x80 only");
		return;
	}

	/* EFER.SCE enables the instruction at all. */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);

	/* STAR holds the selectors. SYSRET derives the user selectors by adding
	 * fixed offsets to the value in bits 63:48, which is why the GDT layout
	 * in gdt.c is not free to change. */
	wrmsr(MSR_STAR, ((u64)(SEL_UDATA - 8) << 48) | ((u64)SEL_KCODE << 32));
	wrmsr(MSR_LSTAR, (u64)(uintptr_t)x86_syscall_entry);

	/* Mask the flags that must not survive into kernel code. Interrupts off
	 * above all: the entry stub runs on a stack it has not switched to yet.
	 * Direction flag cleared, because the ABI says string ops go forward and
	 * a userspace process may have left it set. */
	wrmsr(MSR_SFMASK, (1ull << 9)   /* IF */
	                | (1ull << 10)  /* DF */
	                | (1ull << 8)   /* TF */
	                | (1ull << 18)  /* AC */
	                | (1ull << 16)); /* RF */

	pr_info("SYSCALL entry installed at %p", (void *)x86_syscall_entry);
}
