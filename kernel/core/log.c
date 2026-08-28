/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - structured log.
 *
 * Records go three places at once: the console, a fixed-size ring that dmesg
 * reads, and the runtime graph. The third is the interesting one - the boot
 * narrative becomes queryable state rather than scrollback that is gone the
 * moment the screen wraps.
 */
#include <rk/log.h>
#include <rk/printf.h>
#include <rk/string.h>
#include <rk/console.h>
#include <rk/spinlock.h>
#include <rk/time.h>
#include <rk/graph.h>

#define LOG_RING_BYTES 65536
#define LOG_LINE_MAX   512

static char             ring[LOG_RING_BYTES];
static size_t           ring_head;          /* absolute byte count written */
static DEFINE_SPINLOCK(log_lock);
static enum rk_loglevel current_level = RK_LOG_INFO;
static u64              record_count;
static bool             log_ready;

static const char *const level_name[RK_LOG_NLEVELS] = {
	"emerg", "alert", "crit", "err", "warn", "notice", "info", "debug", "trace"
};

static const u8 level_color[RK_LOG_NLEVELS] = {
	RK_COLOR_LIGHT_RED, RK_COLOR_LIGHT_RED, RK_COLOR_LIGHT_RED, RK_COLOR_RED,
	RK_COLOR_YELLOW, RK_COLOR_LIGHT_CYAN, RK_COLOR_LIGHT_GRAY,
	RK_COLOR_DARK_GRAY, RK_COLOR_DARK_GRAY
};

const char *rk_loglevel_name(enum rk_loglevel lvl)
{
	return (unsigned)lvl < RK_LOG_NLEVELS ? level_name[lvl] : "?";
}

void rk_log_init(void)
{
	spin_lock_init(&log_lock, "log");
	ring_head = 0;
	record_count = 0;
	log_ready = true;
}

void rk_log_set_level(enum rk_loglevel lvl)
{
	if ((unsigned)lvl < RK_LOG_NLEVELS)
		current_level = lvl;
}

enum rk_loglevel rk_log_level(void) { return current_level; }
u64 rk_log_records(void) { return record_count; }

struct sink {
	char  *line;
	size_t cap;
	size_t len;
};

static void sink_putc(void *ctx, char c)
{
	struct sink *s = ctx;
	if (s->len + 1 < s->cap)
		s->line[s->len++] = c;
}

static void ring_append(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		ring[(ring_head + i) % LOG_RING_BYTES] = s[i];
	ring_head += n;
}

void rk_log(enum rk_loglevel lvl, const char *subsys, const char *fmt, ...)
{
	char line[LOG_LINE_MAX];
	struct sink s = { line, sizeof(line), 0 };

	if (lvl > current_level)
		return;

	u64 ns  = rk_time_ns();
	u64 sec = ns / RK_NS_PER_S;
	u64 us  = (ns % RK_NS_PER_S) / 1000;

	rk_fctprintf(sink_putc, &s, "[%5llu.%06llu] %-8s %-7s ",
	             (unsigned long long)sec, (unsigned long long)us,
	             subsys ? subsys : "kernel", rk_loglevel_name(lvl));

	va_list ap;
	va_start(ap, fmt);
	rk_vfctprintf(sink_putc, &s, fmt, ap);
	va_end(ap);

	if (s.len == 0 || line[s.len - 1] != '\n')
		sink_putc(&s, '\n');
	line[s.len] = '\0';

	/* The console write is inside the lock, not after it. On a multiprocessor
	 * machine two cores logging at once otherwise interleave halfway through a
	 * line, and a boot log that reads as garbage is worse than no boot log. */
	unsigned long flags = spin_lock_irqsave(&log_lock);
	if (log_ready) {
		ring_append(line, s.len);
		record_count++;
	}
	rk_console_set_color(level_color[lvl], RK_COLOR_BLACK);
	rk_console_write(line, s.len);
	rk_console_set_color(RK_COLOR_LIGHT_GRAY, RK_COLOR_BLACK);
	spin_unlock_irqrestore(&log_lock, flags);

	/* Only warnings and worse become graph events: an INFO-per-event ring
	 * would drown the causal log in boot chatter. */
	if (lvl <= RK_LOG_WARN)
		rk_graph_record(GEV_LOG, 0, (u64)lvl, record_count, ns);
}

/* Reads forward from an absolute byte cursor. A cursor older than the ring is
 * clamped, so a slow reader loses the oldest bytes instead of reading garbage. */
size_t rk_log_read(char *buf, size_t size, size_t *cursor)
{
	unsigned long flags = spin_lock_irqsave(&log_lock);

	size_t oldest = ring_head > LOG_RING_BYTES ? ring_head - LOG_RING_BYTES : 0;
	size_t pos    = *cursor < oldest ? oldest : *cursor;
	size_t avail  = ring_head > pos ? ring_head - pos : 0;
	size_t n      = avail < size ? avail : size;

	for (size_t i = 0; i < n; i++)
		buf[i] = ring[(pos + i) % LOG_RING_BYTES];

	*cursor = pos + n;
	spin_unlock_irqrestore(&log_lock, flags);
	return n;
}
