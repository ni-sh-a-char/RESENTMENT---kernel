/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - time, clocks and timers.
 *
 * Time is a first-class kernel concept here, not a convenience: Kaalka binds
 * authority to the clock, and the runtime graph binds causality to it. Two
 * clocks are therefore kept deliberately separate:
 *
 *   monotonic  - nanoseconds since boot, never steps, used for scheduling
 *   wall       - UNIX seconds, may step, used for Kaalka seals and logs
 *
 * and a third, virtual clock exists during deterministic replay so a replayed
 * system observes the same timestamps the recorded one did.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>

/* The scheduler tick. 100 Hz is a deliberate middle: fast enough that a 10 ms
 * slice is meaningful, slow enough that the interrupt is not a tax on a
 * machine doing bulk work. Deadline threads use one-shot timers instead, so
 * this rate does not bound their precision. */
#define RK_HZ 100u

#define RK_NS_PER_US 1000ull
#define RK_NS_PER_MS 1000000ull
#define RK_NS_PER_S  1000000000ull

struct rk_walltime {
	s64 unix_sec;
	u32 nsec;
};

/* Broken-down UTC, needed because Kaalka keys off hour:minute:second. */
struct rk_tm {
	int year;   /* absolute, e.g. 2026 */
	int month;  /* 1-12 */
	int day;    /* 1-31 */
	int hour;   /* 0-23 */
	int min;    /* 0-59 */
	int sec;    /* 0-60 */
	int yday;   /* 0-365 */
	int wday;   /* 0=Sunday */
};

void   rk_time_init(void);
u64    rk_time_ns(void);        /* monotonic */
u64    rk_time_ms(void);
u64    rk_uptime_sec(void);
void   rk_walltime(struct rk_walltime *out);
s64    rk_unix_time(void);
void   rk_set_unix_time(s64 sec);

void   rk_gmtime(s64 unix_sec, struct rk_tm *out);
s64    rk_mktime(const struct rk_tm *tm);
size_t rk_format_time(char *buf, size_t n, s64 unix_sec);  /* ISO-8601 UTC */

void   rk_udelay(u64 us);   /* busy wait, safe in early boot and IRQ context */
void   rk_mdelay(u64 ms);

/* ------------------------------------------------------------------ timers */

struct rk_timer;
typedef void (*rk_timer_fn)(struct rk_timer *t, void *arg);

struct rk_timer {
	struct list_head link;
	u64              expires_ns;   /* monotonic deadline */
	u64              period_ns;    /* 0 for one-shot */
	rk_timer_fn      fn;
	void            *arg;
	bool             active;
	const char      *name;
};

void rk_timer_init(struct rk_timer *t, const char *name, rk_timer_fn fn, void *arg);
void rk_timer_start(struct rk_timer *t, u64 delay_ns);
void rk_timer_start_periodic(struct rk_timer *t, u64 period_ns);
void rk_timer_cancel(struct rk_timer *t);

/* Called from the arch timer interrupt. Runs expired callbacks. */
void rk_timer_tick(void);

/* ------------------------------------------------- deterministic replay clock */

/* During replay the recorded timeline is played back instead of the hardware
 * clock, so every timestamp a task observes matches the original run. */
void rk_time_enter_replay(u64 base_mono_ns, s64 base_unix_sec);
void rk_time_replay_advance(u64 mono_ns);
void rk_time_leave_replay(void);
bool rk_time_replaying(void);
