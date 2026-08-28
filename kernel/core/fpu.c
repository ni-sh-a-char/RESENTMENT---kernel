/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - kernel-mode floating point regions.
 *
 * Almost the entire kernel is compiled with the FPU and vector units disabled.
 * That is not conservatism: if the compiler is allowed to emit a vector
 * instruction anywhere, it will eventually emit one in an interrupt handler,
 * and the interrupted thread's register state is then silently corrupted. The
 * bug that produces is intermittent, data-dependent and appears nowhere near
 * its cause.
 *
 * So float lives in exactly one place - the AI subsystem, which is compiled
 * with SIMD on - and every such region is bracketed by these two calls, which
 * save the interrupted state and hold off preemption until it is restored.
 */
#include <rk/arch.h>
#include <rk/sched.h>
#include <rk/panic.h>
#include <rk/irq.h>

/* Depth is per-CPU rather than per-thread because preemption is disabled for
 * the whole region, so the region cannot migrate. */
static u32 fpu_depth[RK_MAX_CPUS];

void rk_fpu_begin(void)
{
	RK_ASSERT_MSG(!rk_in_irq(), "floating point in interrupt context");

	u32 cpu = arch_cpu_id();
	if (cpu >= RK_MAX_CPUS)
		return;

	if (fpu_depth[cpu]++ == 0) {
		sched_preempt_disable();
		struct thread *t = arch_current_thread();
		if (t)
			arch_fpu_save(t);
	}
}

void rk_fpu_end(void)
{
	u32 cpu = arch_cpu_id();
	if (cpu >= RK_MAX_CPUS || !fpu_depth[cpu])
		return;

	if (--fpu_depth[cpu] == 0) {
		struct thread *t = arch_current_thread();
		if (t)
			arch_fpu_restore(t);
		sched_preempt_enable();
	}
}

bool rk_fpu_active(void)
{
	u32 cpu = arch_cpu_id();
	return cpu < RK_MAX_CPUS && fpu_depth[cpu] > 0;
}
