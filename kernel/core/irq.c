/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - interrupt dispatch.
 *
 * Shared lines are supported because PCI requires it. Threaded handlers are
 * supported because a driver that does real work in hard IRQ context adds that
 * work to the worst-case latency of every real-time and inference task on the
 * machine, and this kernel makes promises about both.
 */
#include <rk/irq.h>
#include <rk/log.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/spinlock.h>
#include <rk/printf.h>
#include <rk/arch.h>
#include <rk/time.h>
#include <rk/sched.h>
#include <rk/graph.h>

#undef RK_SUBSYS
#define RK_SUBSYS "irq"

struct irq_desc {
	struct list_head actions;
	spinlock_t       lock;
	u64              count;
	u64              spurious;
	u64              ns_total;
	u64              ns_max;
	bool             masked;
};

static struct irq_desc irqs[RK_MAX_IRQ];
static volatile u32 in_irq_depth[RK_MAX_CPUS];

/* Vector numbering: the architecture hands us its own vector, and lines 0..31
 * on x86 are CPU exceptions, so device IRQ n arrives as vector 32+n. Keeping
 * the translation here means drivers ask for line numbers, not vectors. */
#ifdef RK_ARCH_X86_64
#define IRQ_VECTOR_BASE 32u
#else
#define IRQ_VECTOR_BASE 0u
#endif

void rk_irq_init(void)
{
	for (u32 i = 0; i < RK_MAX_IRQ; i++) {
		list_init(&irqs[i].actions);
		spin_lock_init(&irqs[i].lock, "irq");
		irqs[i].masked = true;
	}
	pr_info("interrupt dispatch ready, %u lines", RK_MAX_IRQ);
}

int rk_irq_request(u32 irq, rk_irq_fn handler, rk_irq_fn thread_fn,
                   void *dev, const char *name)
{
	if (irq >= RK_MAX_IRQ || !handler)
		return RK_EINVAL;

	struct rk_irqaction *a = kzalloc(sizeof(*a));
	if (!a)
		return RK_ENOMEM;
	a->handler   = handler;
	a->thread_fn = thread_fn;
	a->dev       = dev;
	a->name      = name;

	unsigned long f = spin_lock_irqsave(&irqs[irq].lock);
	list_add_tail(&a->link, &irqs[irq].actions);
	spin_unlock_irqrestore(&irqs[irq].lock, f);

	pr_debug("line %u -> %s", irq, name ? name : "?");
	return RK_OK;
}

int rk_irq_free(u32 irq, void *dev)
{
	if (irq >= RK_MAX_IRQ)
		return RK_EINVAL;

	int rc = RK_ENOENT;
	unsigned long f = spin_lock_irqsave(&irqs[irq].lock);
	struct rk_irqaction *a, *tmp;
	list_for_each_entry_safe(a, tmp, &irqs[irq].actions, link) {
		if (a->dev == dev) {
			list_del(&a->link);
			kfree(a);
			rc = RK_OK;
		}
	}
	bool empty = list_empty(&irqs[irq].actions);
	spin_unlock_irqrestore(&irqs[irq].lock, f);

	if (empty)
		rk_irq_mask(irq);
	return rc;
}

void rk_irq_mask(u32 irq)
{
	if (irq >= RK_MAX_IRQ)
		return;
	irqs[irq].masked = true;
	rk_irq_mask_arch(irq, true);
}

void rk_irq_unmask(u32 irq)
{
	if (irq >= RK_MAX_IRQ)
		return;
	irqs[irq].masked = false;
	rk_irq_mask_arch(irq, false);
}

bool rk_in_irq(void)
{
	u32 cpu = arch_cpu_id();
	return cpu < RK_MAX_CPUS && in_irq_depth[cpu] > 0;
}

void rk_irq_dispatch(u32 vector)
{
	u32 cpu = arch_cpu_id();
	u32 line = vector >= IRQ_VECTOR_BASE ? vector - IRQ_VECTOR_BASE : vector;

	if (cpu < RK_MAX_CPUS)
		in_irq_depth[cpu]++;

	u64 start = rk_time_ns();
	bool handled = false;
	bool want_thread = false;

	if (line < RK_MAX_IRQ) {
		struct irq_desc *d = &irqs[line];
		struct rk_irqaction *a;

		/* No lock: the action list is only mutated with interrupts disabled on
		 * this CPU, and taking a lock here would put the interrupt controller
		 * behind a contended cache line on every device interrupt. */
		list_for_each_entry(a, &d->actions, link) {
			enum rk_irq_result r = a->handler(line, a->dev);
			a->count++;
			if (r == RK_IRQ_HANDLED) {
				handled = true;
			} else if (r == RK_IRQ_WAKE_THREAD) {
				handled = true;
				want_thread = true;
			}
		}
		d->count++;
		u64 elapsed = rk_time_ns() - start;
		d->ns_total += elapsed;
		if (elapsed > d->ns_max)
			d->ns_max = elapsed;
		if (!handled) {
			d->spurious++;
			/* A line nobody claims will otherwise fire forever. Masking it
			 * loses the device but keeps the machine usable, which is the
			 * right trade for an interrupt storm. */
			if (d->spurious == 1000) {
				pr_err("line %u unclaimed 1000 times, masking", line);
				rk_irq_mask(line);
			}
		}
	}

	rk_irq_eoi_arch(vector);

	if (cpu < RK_MAX_CPUS)
		in_irq_depth[cpu]--;

	rk_graph_record(GEV_IRQ, 0, line, handled ? 1 : 0, rk_time_ns() - start);

	if (want_thread) {
		/* Threaded halves run after the EOI so the line is live again while
		 * they work. */
		if (line < RK_MAX_IRQ) {
			struct rk_irqaction *a;
			list_for_each_entry(a, &irqs[line].actions, link)
				if (a->thread_fn)
					a->thread_fn(line, a->dev);
		}
	}

	/* The timer interrupt is what drives preemption, and this is the only
	 * place the kernel knows a slice may have expired. */
	if (line == 0 && sched_active())
		sched_tick();
}

void rk_irq_stats(u32 irq, u64 *count, u64 *ns_total)
{
	if (irq >= RK_MAX_IRQ)
		return;
	if (count)
		*count = irqs[irq].count;
	if (ns_total)
		*ns_total = irqs[irq].ns_total;
}

size_t rk_irq_dump(char *buf, size_t cap)
{
	size_t n = 0;
	n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
	                      "%4s %10s %10s %12s  %s\n",
	                      "line", "count", "spurious", "avg ns", "handlers");
	for (u32 i = 0; i < RK_MAX_IRQ; i++) {
		if (!irqs[i].count && list_empty(&irqs[i].actions))
			continue;
		u64 avg = irqs[i].count ? irqs[i].ns_total / irqs[i].count : 0;
		n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
		                      "%4u %10llu %10llu %12llu  ", i,
		                      (unsigned long long)irqs[i].count,
		                      (unsigned long long)irqs[i].spurious,
		                      (unsigned long long)avg);
		struct rk_irqaction *a;
		list_for_each_entry(a, &irqs[i].actions, link)
			n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "%s ",
			                      a->name ? a->name : "?");
		n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "\n");
	}
	return n;
}
