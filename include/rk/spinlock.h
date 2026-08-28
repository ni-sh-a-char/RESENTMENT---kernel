/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - ticket spinlocks.
 *
 * Ticket (FIFO) rather than test-and-set: on a many-core machine a TAS lock
 * starves whichever core is furthest from the cache line, which shows up as
 * unbounded latency exactly where the kernel promises bounds (IRQ paths).
 */
#pragma once

#include <rk/types.h>
#include <rk/atomic.h>
#include <rk/compiler.h>
#include <rk/arch.h>

typedef struct {
	atomic32_t head;   /* next ticket to be served */
	atomic32_t tail;   /* next ticket to be handed out */
	u32        owner;  /* cpu id holding it, for deadlock diagnostics */
	const char *name;
} spinlock_t;

#define SPINLOCK_INIT(nm) { ATOMIC_INIT(0), ATOMIC_INIT(0), (u32)-1, nm }
#define DEFINE_SPINLOCK(v) spinlock_t v = SPINLOCK_INIT(#v)

static inline void spin_lock_init(spinlock_t *l, const char *name)
{
	atomic32_store(&l->head, 0);
	atomic32_store(&l->tail, 0);
	l->owner = (u32)-1;
	l->name = name;
}

static inline void spin_lock(spinlock_t *l)
{
	u32 ticket = atomic32_add(&l->tail, 1) - 1;
	while (atomic32_load(&l->head) != ticket)
		arch_cpu_relax();
	l->owner = arch_cpu_id();
}

static inline bool spin_trylock(spinlock_t *l)
{
	u32 head = atomic32_load(&l->head);
	u32 tail = atomic32_load(&l->tail);
	if (head != tail)
		return false;
	if (!atomic32_cas(&l->tail, &tail, tail + 1))
		return false;
	l->owner = arch_cpu_id();
	return true;
}

static inline void spin_unlock(spinlock_t *l)
{
	l->owner = (u32)-1;
	atomic32_add(&l->head, 1);
}

/* IRQ-safe variants. Any lock also taken from an interrupt handler must be
 * acquired with these, or the handler can deadlock against the holder. */
static inline unsigned long spin_lock_irqsave(spinlock_t *l)
{
	unsigned long flags = arch_irq_save();
	spin_lock(l);
	return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags)
{
	spin_unlock(l);
	arch_irq_restore(flags);
}
