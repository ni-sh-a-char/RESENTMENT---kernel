/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - x86_64 timekeeping.
 *
 * The TSC is the only clock fast enough to timestamp every graph event, but it
 * is only trustworthy if the CPU advertises an invariant one. So: calibrate
 * the TSC against the PIT once, use it for all subsequent reads, and fall back
 * to counting PIT ticks on hardware where the TSC drifts with frequency.
 */
#include <arch/x86.h>
#include <rk/time.h>
#include <rk/log.h>
#include <rk/irq.h>
#include <rk/arch.h>

#undef RK_SUBSYS
#define RK_SUBSYS "time"

#define PIT_CHAN0 0x40
#define PIT_CHAN2 0x42
#define PIT_CMD   0x43
#define PIT_HZ    1193182u

u64 x86_tsc_hz;
static bool tsc_invariant;
static u64  tsc_boot;
static volatile u64 pit_ticks;
static u32  pit_hz;

/* Channel 2 is wired to the speaker gate rather than to an interrupt, which
 * makes it the one PIT channel we can poll without disturbing the tick. */
static u64 pit_measure_tsc(u32 ms)
{
	u16 count = (u16)((u64)PIT_HZ * ms / 1000);
	if (!count)
		count = 1;

	u8 port61 = arch_inb(0x61);
	arch_outb(0x61, (u8)((port61 & ~0x02) | 0x01));   /* gate on, speaker off */

	arch_outb(PIT_CMD, 0xB2);          /* channel 2, lobyte/hibyte, mode 0 */
	arch_outb(PIT_CHAN2, (u8)(count & 0xFF));
	arch_outb(PIT_CHAN2, (u8)(count >> 8));

	/* Restart the counter by toggling the gate. */
	arch_outb(0x61, (u8)(port61 & ~0x01));
	arch_outb(0x61, (u8)((port61 & ~0x02) | 0x01));

	u64 start = rdtsc();
	while (!(arch_inb(0x61) & 0x20))
		arch_cpu_relax();
	u64 end = rdtsc();

	arch_outb(0x61, port61);
	return end - start;
}

u64 x86_pit_calibrate_tsc(void)
{
	/* Three runs, take the median: SMI and virtualisation exits routinely
	 * corrupt a single measurement, and a wrong clock rate poisons every
	 * timeout in the system. */
	u64 s[3];
	for (int i = 0; i < 3; i++)
		s[i] = pit_measure_tsc(10) * 100;   /* scale 10 ms to 1 s */

	for (int i = 0; i < 2; i++)
		for (int j = i + 1; j < 3; j++)
			if (s[j] < s[i]) { u64 t = s[i]; s[i] = s[j]; s[j] = t; }

	x86_tsc_hz = s[1];
	if (x86_tsc_hz < 1000000ull)
		x86_tsc_hz = 1000000000ull;        /* implausible, assume 1 GHz */
	return x86_tsc_hz;
}

static enum rk_irq_result pit_handler(u32 irq, void *dev)
{
	(void)irq; (void)dev;
	pit_ticks++;
	rk_timer_tick();
	return RK_IRQ_HANDLED;
}

void x86_pit_init(u32 hz)
{
	pit_hz = hz ? hz : 100;
	u16 div = (u16)(PIT_HZ / pit_hz);
	arch_outb(PIT_CMD, 0x36);          /* channel 0, lobyte/hibyte, mode 3 */
	arch_outb(PIT_CHAN0, (u8)(div & 0xFF));
	arch_outb(PIT_CHAN0, (u8)(div >> 8));
	rk_irq_request(0, pit_handler, NULL, NULL, "pit");
	rk_irq_unmask(0);
}

void x86_time_init(void)
{
	u32 a, b, c, d;
	cpuid_raw(0x80000007, 0, &a, &b, &c, &d);
	tsc_invariant = (d & (1u << 8)) != 0;

	x86_pit_calibrate_tsc();
	tsc_boot = rdtsc();

	pr_info("TSC %llu.%03llu MHz%s",
	        (unsigned long long)(x86_tsc_hz / 1000000),
	        (unsigned long long)((x86_tsc_hz / 1000) % 1000),
	        tsc_invariant ? " (invariant)" : " (variable, timing may drift)");
}

u64 arch_cycles(void) { return rdtsc(); }
u64 arch_cycles_per_sec(void) { return x86_tsc_hz ? x86_tsc_hz : 1000000000ull; }

u64 arch_time_ns(void)
{
	if (!x86_tsc_hz)
		return 0;
	u64 delta = rdtsc() - tsc_boot;
	/* 128-bit intermediate: at 3 GHz a 64-bit product overflows after six
	 * seconds of uptime, which is exactly the kind of bug that only shows up
	 * once the machine has been running long enough to matter. */
	return (u64)(((__uint128_t)delta * 1000000000ull) / x86_tsc_hz);
}

/* rk_udelay lives in kernel/core/time.c: every architecture has a cycle
 * counter and a rate, so one implementation serves all three. */

/* ---------------------------------------------------------------- CMOS RTC */

static u8 cmos_read(u8 reg)
{
	arch_outb(0x70, (u8)(0x80 | reg));   /* NMI disabled while reading */
	return arch_inb(0x71);
}

static inline int bcd(u8 v) { return (v & 0x0F) + ((v >> 4) * 10); }

s64 arch_wallclock_unix(void)
{
	/* Wait out an update in progress, otherwise the fields can be torn. */
	int guard = 1000000;
	while ((cmos_read(0x0A) & 0x80) && guard--)
		arch_cpu_relax();

	u8 statusb = cmos_read(0x0B);
	bool binary = (statusb & 0x04) != 0;
	bool hour24 = (statusb & 0x02) != 0;

	u8 sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
	u8 day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);
	u8 century = cmos_read(0x32);

	struct rk_tm tm;
	bool pm = !hour24 && (hour & 0x80);
	hour &= 0x7F;

	tm.sec  = binary ? sec  : bcd(sec);
	tm.min  = binary ? min  : bcd(min);
	tm.hour = binary ? hour : bcd(hour);
	tm.day  = binary ? day  : bcd(day);
	tm.month = binary ? mon : bcd(mon);
	int yy  = binary ? year : bcd(year);
	int cc  = binary ? century : bcd(century);
	if (cc < 19 || cc > 25)
		cc = 20;                    /* garbage century register, assume 2000s */
	tm.year = cc * 100 + yy;
	if (!hour24 && pm && tm.hour != 12)
		tm.hour += 12;
	tm.yday = tm.wday = 0;

	if (tm.month < 1 || tm.month > 12 || tm.day < 1 || tm.day > 31)
		return 0;
	return rk_mktime(&tm);
}

void arch_timer_init(u32 hz)
{
	if (x86_using_apic()) {
		x86_apic_timer_init(hz);
		/* The PIT still drives rk_timer_tick until the APIC vector is wired
		 * to it; both land on vector 0x20, so only one fires. */
		rk_irq_request(0, pit_handler, NULL, NULL, "apic-timer");
	} else {
		x86_pit_init(hz);
	}
}

void arch_timer_oneshot(u64 ns)
{
	/* TSC-deadline where available: one MSR write instead of reprogramming a
	 * divider, and it is exact rather than quantised to the tick. */
	u32 a, b, c, d;
	cpuid_raw(1, 0, &a, &b, &c, &d);
	if (c & (1u << 24)) {
		u64 deadline = rdtsc() + (u64)(((__uint128_t)ns * x86_tsc_hz) / 1000000000ull);
		wrmsr(MSR_TSC_DEADLINE, deadline);
	}
}

u64 x86_pit_ticks(void) { return pit_ticks; }
