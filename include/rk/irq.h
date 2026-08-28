/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - interrupt dispatch.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>

#define RK_MAX_IRQ 256

enum rk_irq_result {
	RK_IRQ_NONE = 0,     /* not my device */
	RK_IRQ_HANDLED,      /* fully handled in hard IRQ context */
	RK_IRQ_WAKE_THREAD,  /* defer the rest to the IRQ thread */
};

typedef enum rk_irq_result (*rk_irq_fn)(u32 irq, void *dev);

struct rk_irqaction {
	struct list_head link;
	rk_irq_fn        handler;
	rk_irq_fn        thread_fn;
	void            *dev;
	const char      *name;
	u64              count;
	u64              ns_total;   /* time spent, for latency accounting */
};

void rk_irq_init(void);
int  rk_irq_request(u32 irq, rk_irq_fn handler, rk_irq_fn thread_fn,
                    void *dev, const char *name);
int  rk_irq_free(u32 irq, void *dev);
void rk_irq_mask(u32 irq);
void rk_irq_unmask(u32 irq);

/* Entry point from arch trap code. */
void rk_irq_dispatch(u32 irq);

/* Tell the dispatcher which line drives preemption. Defaults to zero,
 * which is right for a PC and wrong for almost everything else. */
void rk_irq_set_timer_line(u32 line);

/* Implemented per architecture by whatever interrupt controller is in use. */
void rk_irq_mask_arch(u32 irq, bool masked);
void rk_irq_eoi_arch(u32 vector);

/* Are we in hard interrupt context? Allocation and sleeping are illegal here. */
bool rk_in_irq(void);
void rk_irq_stats(u32 irq, u64 *count, u64 *ns_total);
