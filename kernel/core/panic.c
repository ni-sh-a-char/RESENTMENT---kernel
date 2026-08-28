/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - panic.
 *
 * A panic prints what happened, then the one thing a normal kernel cannot: the
 * runtime graph root digest and the tail of the causal event log. Those two
 * together turn "it crashed on some machine" into a reproducible run, because
 * the digest identifies the exact state and the event log replays into it.
 */
#include <rk/panic.h>
#include <rk/printf.h>
#include <rk/console.h>
#include <rk/log.h>
#include <rk/arch.h>
#include <rk/graph.h>
#include <rk/sched.h>
#include <rk/string.h>
#include <rk/atomic.h>

static atomic32_t panicking = ATOMIC_INIT(0);

static void panic_putc(void *ctx, char c)
{
	(void)ctx;
	rk_console_putc(c);
}

static void dump_recent_events(void)
{
	struct graph_event ev[16];
	u64 seq = rk_graph_event_seq();
	u64 cursor = seq > 16 ? seq - 16 : 0;
	size_t n = rk_graph_events_read(ev, 16, &cursor);

	if (!n)
		return;
	rk_printf("\nlast %u causal events:\n", (unsigned)n);
	for (size_t i = 0; i < n; i++)
		rk_printf("  #%llu %10llu ns cpu%u tid%llu %-14s %llx %llx %llx\n",
		          (unsigned long long)ev[i].seq, (unsigned long long)ev[i].mono_ns,
		          (unsigned)ev[i].cpu, (unsigned long long)ev[i].tid,
		          graph_event_kind_name((enum graph_event_kind)ev[i].kind),
		          (unsigned long long)ev[i].a, (unsigned long long)ev[i].b,
		          (unsigned long long)ev[i].c);
}

void rk_panic(const char *file, int line, const char *fmt, ...)
{
	arch_irq_disable();

	/* A second CPU panicking while we print would interleave garbage over the
	 * only diagnostic anyone will ever get. It stops instead. */
	u32 expected = 0;
	if (!atomic32_cas(&panicking, &expected, 1))
		arch_halt();

	rk_console_set_color(RK_COLOR_WHITE, RK_COLOR_RED);
	rk_printf("\n\n*** RESENTMENT KERNEL PANIC ***\n");
	rk_console_set_color(RK_COLOR_LIGHT_RED, RK_COLOR_BLACK);

	rk_printf("at %s:%d on cpu%u\n", file, line, arch_cpu_id());

	va_list ap;
	va_start(ap, fmt);
	rk_vfctprintf(panic_putc, NULL, fmt, ap);
	va_end(ap);
	rk_printf("\n");

	struct thread *t = arch_current_thread();
	if (t)
		rk_printf("thread  tid=%llu name=%s state=%s\n",
		          (unsigned long long)t->tid, t->name,
		          thread_state_name((enum thread_state)t->state));

	const u8 *d = rk_graph_root_digest();
	if (d) {
		char hex[65];
		for (int i = 0; i < 32; i++) {
			static const char h[] = "0123456789abcdef";
			hex[i * 2]     = h[d[i] >> 4];
			hex[i * 2 + 1] = h[d[i] & 0xf];
		}
		hex[64] = '\0';
		rk_printf("graph   root=%s\n", hex);
		rk_printf("        replay with: resentment --replay <snapshot> --expect %.16s\n", hex);
	}

	dump_recent_events();

	rk_console_set_color(RK_COLOR_LIGHT_GRAY, RK_COLOR_BLACK);
	rk_printf("\nsystem halted.\n");
	arch_halt();
}
