/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - x86_64 thread bring-up.
 */
#include <arch/x86.h>
#include <rk/arch.h>
#include <rk/sched.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/panic.h>
#include <rk/errno.h>

extern void x86_thread_trampoline(void);

/* Called from the trampoline with the entry point and argument still live in
 * the registers the context switch restored. */
void x86_thread_entry(void (*entry)(void *), void *arg)
{
	sched_finish_switch();
	arch_irq_enable();
	entry(arg);
	thread_exit(0);
}

void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg,
                      vaddr_t stack_top)
{
	/* Stack alignment, which is worth spelling out because getting it wrong by
	 * eight bytes produces a general protection fault on the first aligned SSE
	 * instruction the thread executes - a long way from the cause.
	 *
	 * The SysV ABI requires rsp % 16 == 0 immediately *before* a call, so a
	 * function sees rsp % 16 == 8 at its first instruction. The trampoline is
	 * reached by the `ret` at the end of arch_context_switch, and it then
	 * makes a call of its own. So the frame is built such that the trampoline
	 * starts with rsp % 16 == 0.
	 *
	 * Ten slots: two zeroes to terminate the frame chain for a backtrace, the
	 * trampoline address for the ret to land on, then exactly what
	 * arch_context_switch pops.
	 */
	u64 *sp = (u64 *)ALIGN_DOWN(stack_top, 16);

	*--sp = 0;                                       /* frame chain terminator */
	*--sp = 0;
	*--sp = (u64)(uintptr_t)x86_thread_trampoline;   /* what the ret lands on */
	*--sp = 0;                                       /* rbp */
	*--sp = 0;                                       /* rbx */
	*--sp = (u64)(uintptr_t)entry;                   /* r12 */
	*--sp = (u64)(uintptr_t)arg;                     /* r13 */
	*--sp = 0;                                       /* r14 */
	*--sp = 0;                                       /* r15 */
	/* Interrupts start disabled. x86_thread_entry enables them as its first
	 * act, which keeps the window between "switched in" and "ready to be
	 * interrupted" closed rather than opening it inside the scheduler. */
	*--sp = 0x002;                                   /* rflags: reserved bit 1 */

	RK_ASSERT_MSG((((uintptr_t)sp + 7 * 8 + 8) & 15) == 0,
	              "thread entry stack alignment is wrong");

	t->sp = sp;

	/* FXSAVE and XSAVE both fault on a misaligned target, so ask the heap for
	 * the alignment rather than adjusting the pointer afterwards and losing
	 * the one kfree can free. */
	if (arch_cpu_features() & RK_FEAT_FPU)
		t->fpu_state = kmalloc_aligned(arch_fpu_state_size(), 64);
}

void arch_thread_free(struct thread *t)
{
	if (t->fpu_state) {
		kfree(t->fpu_state);
		t->fpu_state = NULL;
	}
}

int arch_enter_user(vaddr_t entry, vaddr_t stack, void *arg)
{
	struct thread *t = arch_current_thread();
	if (t)
		x86_tss_set_kernel_stack(t->kstack + t->kstack_size);

	/* Hand-built iret frame: this is the only way into ring 3 that also sets
	 * rflags and the stack atomically. */
	__asm__ __volatile__(
		"cli\n\t"
		"pushq %[uds]\n\t"
		"pushq %[ustack]\n\t"
		"pushq $0x202\n\t"
		"pushq %[ucs]\n\t"
		"pushq %[uentry]\n\t"
		"movq  %[arg], %%rdi\n\t"
		"swapgs\n\t"
		"iretq\n\t"
		:
		: [uds] "i"((u64)SEL_UDATA), [ucs] "i"((u64)SEL_UCODE),
		  [ustack] "r"((u64)stack), [uentry] "r"((u64)entry), [arg] "r"(arg)
		: "memory");
	return RK_OK;   /* not reached */
}
