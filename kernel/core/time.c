/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - portable timekeeping and timers.
 *
 * The replay clock is the reason this file exists rather than every caller
 * asking the architecture directly. During deterministic replay the kernel
 * must hand out the timestamps that were recorded, not the ones the hardware
 * is producing now, or nothing downstream reproduces - including Kaalka seals,
 * which are keyed by wall time.
 */
#include <rk/time.h>
#include <rk/arch.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/string.h>
#include <rk/printf.h>
#include <rk/spinlock.h>
#include <rk/list.h>

#undef RK_SUBSYS
#define RK_SUBSYS "time"

static LIST_HEAD(timers);
static DEFINE_SPINLOCK(timer_lock);

static s64  wall_base_unix;     /* wall time at monotonic zero */
static bool wall_valid;

static bool replaying;
static u64  replay_mono;
static s64  replay_unix;

void rk_time_init(void)
{
	spin_lock_init(&timer_lock, "timers");
	s64 now = arch_wallclock_unix();
	if (now > 0) {
		wall_base_unix = now - (s64)(arch_time_ns() / RK_NS_PER_S);
		wall_valid = true;
		char buf[40];
		rk_format_time(buf, sizeof(buf), now);
		pr_info("wall clock: %s", buf);
	} else {
		pr_warn("no usable RTC; wall time starts at the epoch");
	}
}

u64 rk_time_ns(void)
{
	if (replaying)
		return replay_mono;
	return arch_time_ns();
}

u64 rk_time_ms(void)     { return rk_time_ns() / RK_NS_PER_MS; }
u64 rk_uptime_sec(void)  { return rk_time_ns() / RK_NS_PER_S; }

s64 rk_unix_time(void)
{
	if (replaying)
		return replay_unix;
	return wall_base_unix + (s64)(arch_time_ns() / RK_NS_PER_S);
}

void rk_set_unix_time(s64 sec)
{
	wall_base_unix = sec - (s64)(arch_time_ns() / RK_NS_PER_S);
	wall_valid = true;
}

void rk_walltime(struct rk_walltime *out)
{
	u64 ns = rk_time_ns();
	out->unix_sec = rk_unix_time();
	out->nsec     = (u32)(ns % RK_NS_PER_S);
}

/* Busy wait. Legal in interrupt context and in early boot, which is why it
 * spins on the cycle counter rather than sleeping. Every architecture exposes
 * a counter and its rate, so this needs no per-architecture version. */
void rk_udelay(u64 us)
{
	u64 hz = arch_cycles_per_sec();
	if (!hz) {
		/* Before the counter is calibrated, spin a bounded number of hint
		 * instructions. Imprecise, but this only happens during bring-up. */
		for (u64 i = 0; i < us * 100; i++)
			arch_cpu_relax();
		return;
	}
	u64 target = arch_cycles() + (u64)(((__uint128_t)us * hz) / 1000000ull);
	while ((s64)(arch_cycles() - target) < 0)
		arch_cpu_relax();
}

void rk_mdelay(u64 ms) { rk_udelay(ms * 1000); }

/* ---------------------------------------------------------------- timers */

void rk_timer_init(struct rk_timer *t, const char *name, rk_timer_fn fn, void *arg)
{
	list_init(&t->link);
	t->name       = name;
	t->fn         = fn;
	t->arg        = arg;
	t->active     = false;
	t->period_ns  = 0;
	t->expires_ns = 0;
}

/* The list is kept sorted by deadline. A sorted list is O(n) to insert, which
 * is fine while the kernel has tens of timers; if that ever becomes hundreds,
 * this is the place to grow a hierarchical wheel.
 * ponytail: sorted list, upgrade to a timer wheel if insertion shows up hot. */
static void timer_insert(struct rk_timer *t)
{
	struct rk_timer *p;
	list_for_each_entry(p, &timers, link) {
		if (t->expires_ns < p->expires_ns) {
			__list_add(&t->link, p->link.prev, &p->link);
			t->active = true;
			return;
		}
	}
	list_add_tail(&t->link, &timers);
	t->active = true;
}

void rk_timer_start(struct rk_timer *t, u64 delay_ns)
{
	unsigned long f = spin_lock_irqsave(&timer_lock);
	if (t->active)
		list_del(&t->link);
	t->expires_ns = rk_time_ns() + delay_ns;
	t->period_ns  = 0;
	timer_insert(t);
	spin_unlock_irqrestore(&timer_lock, f);
}

void rk_timer_start_periodic(struct rk_timer *t, u64 period_ns)
{
	unsigned long f = spin_lock_irqsave(&timer_lock);
	if (t->active)
		list_del(&t->link);
	t->period_ns  = period_ns;
	t->expires_ns = rk_time_ns() + period_ns;
	timer_insert(t);
	spin_unlock_irqrestore(&timer_lock, f);
}

void rk_timer_cancel(struct rk_timer *t)
{
	unsigned long f = spin_lock_irqsave(&timer_lock);
	if (t->active) {
		list_del(&t->link);
		t->active = false;
	}
	spin_unlock_irqrestore(&timer_lock, f);
}

void rk_timer_tick(void)
{
	u64 now = rk_time_ns();

	for (;;) {
		unsigned long f = spin_lock_irqsave(&timer_lock);
		if (list_empty(&timers)) {
			spin_unlock_irqrestore(&timer_lock, f);
			break;
		}
		struct rk_timer *t = list_first_entry(&timers, struct rk_timer, link);
		if (t->expires_ns > now) {
			spin_unlock_irqrestore(&timer_lock, f);
			break;
		}
		list_del(&t->link);
		t->active = false;
		if (t->period_ns) {
			t->expires_ns = now + t->period_ns;
			timer_insert(t);
		}
		rk_timer_fn fn = t->fn;
		void *arg = t->arg;
		spin_unlock_irqrestore(&timer_lock, f);

		/* Callback runs unlocked: a timer that starts another timer is normal
		 * and must not deadlock. */
		if (fn)
			fn(t, arg);
	}
}

/* --------------------------------------------------------- replay clock */

void rk_time_enter_replay(u64 base_mono_ns, s64 base_unix_sec)
{
	replay_mono = base_mono_ns;
	replay_unix = base_unix_sec;
	replaying   = true;
	pr_notice("clock entering replay at %llu ns", (unsigned long long)base_mono_ns);
}

void rk_time_replay_advance(u64 mono_ns)
{
	if (!replaying)
		return;
	/* Time only moves forward, even in a recording: a recorded stream with a
	 * backwards timestamp is corrupt, and honouring it would break every
	 * timeout in the system. */
	if (mono_ns > replay_mono) {
		u64 delta = mono_ns - replay_mono;
		replay_mono = mono_ns;
		replay_unix += (s64)(delta / RK_NS_PER_S);
		rk_timer_tick();
	}
}

void rk_time_leave_replay(void)
{
	replaying = false;
	pr_notice("clock leaving replay");
}

bool rk_time_replaying(void) { return replaying; }
