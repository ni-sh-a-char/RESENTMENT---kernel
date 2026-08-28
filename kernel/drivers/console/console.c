/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - console multiplexer.
 *
 * Output fans out to every registered backend. That is not redundancy for its
 * own sake: a headless board has only a UART, a laptop booted from GRUB has
 * only a framebuffer, and a developer wants both. Registering more than one
 * costs a pointer walk per character and removes an entire class of "it hangs
 * before anything prints" debugging.
 */
#include <rk/console.h>
#include <rk/boot.h>
#include <rk/printf.h>
#include <rk/string.h>
#include <rk/spinlock.h>
#include <rk/ringbuf.h>
#include <rk/list.h>
#include <rk/sched.h>
#include <rk/arch.h>

static LIST_HEAD(consoles);
static DEFINE_SPINLOCK(console_lock);

static u8 input_storage[512];
static struct ringbuf input_ring;
static bool input_ready;

int rk_console_register(struct rk_console *c)
{
	if (!c || (!c->putc && !c->write))
		return -1;
	unsigned long f = spin_lock_irqsave(&console_lock);
	c->enabled = true;
	list_add_tail(&c->link, &consoles);
	spin_unlock_irqrestore(&console_lock, f);
	return 0;
}

/* Before any console has registered, output goes straight out the serial port.
 * Without this, a fault during early bring-up is completely silent - which is
 * the single worst thing that can happen while porting, because the machine
 * gives you nothing at all to work from. */
static void early_write(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\n')
			rk_serial_putc('\r');
		rk_serial_putc(s[i]);
	}
}

void rk_console_putc(char ch)
{
	if (list_empty(&consoles)) {
		early_write(&ch, 1);
		return;
	}

	struct rk_console *c;
	list_for_each_entry(c, &consoles, link) {
		if (!c->enabled)
			continue;
		if (c->write)
			c->write(c, &ch, 1);
		else
			c->putc(c, ch);
	}
}

void rk_console_write(const char *s, size_t n)
{
	if (list_empty(&consoles)) {
		early_write(s, n);
		return;
	}

	struct rk_console *c;
	list_for_each_entry(c, &consoles, link) {
		if (!c->enabled)
			continue;
		if (c->write) {
			c->write(c, s, n);
		} else {
			for (size_t i = 0; i < n; i++)
				c->putc(c, s[i]);
		}
	}
}

void rk_console_puts(const char *s) { rk_console_write(s, strlen(s)); }

void rk_console_clear(void)
{
	struct rk_console *c;
	list_for_each_entry(c, &consoles, link)
		if (c->enabled && c->clear)
			c->clear(c);
}

void rk_console_set_color(u8 fg, u8 bg)
{
	struct rk_console *c;
	list_for_each_entry(c, &consoles, link)
		if (c->enabled && c->set_color)
			c->set_color(c, fg, bg);
}

static void printf_putc(void *ctx, char c)
{
	(void)ctx;
	rk_console_putc(c);
}

int rk_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = rk_vfctprintf(printf_putc, NULL, fmt, ap);
	va_end(ap);
	return n;
}

/* ------------------------------------------------------------------ input */

void rk_console_push_input(char c)
{
	if (input_ready)
		ringbuf_put(&input_ring, (u8)c);
}

/* The ring first, because an interrupt-driven console has already queued its
 * bytes there and that path has the lowest latency. Then a poll of every
 * console that offers one, so input still works on a machine where the
 * interrupt never arrives. */
int rk_console_getchar(void)
{
	u8 c;
	if (input_ready && ringbuf_get(&input_ring, &c))
		return (int)c;

	struct rk_console *con;
	list_for_each_entry(con, &consoles, link) {
		if (!con->enabled || !con->getc)
			continue;
		int got = con->getc(con);
		if (got >= 0)
			return got;
	}
	return -1;
}

/* Line editing with backspace. Deliberately minimal: the real line editor is
 * in the shell, where it can see history and completions. */
int rk_console_readline(char *buf, size_t n)
{
	size_t len = 0;

	if (n == 0)
		return 0;
	for (;;) {
		int c = rk_console_getchar();
		if (c < 0) {
			/* Idle rather than spin. Before the scheduler exists this is a
			 * halt-until-interrupt, which is exactly what early boot wants. */
			if (sched_active())
				sched_yield();
			else
				arch_idle();
			continue;
		}
		if (c == '\r' || c == '\n') {
			rk_console_putc('\n');
			break;
		}
		if (c == '\b' || c == 127) {
			if (len) {
				len--;
				rk_console_puts("\b \b");
			}
			continue;
		}
		if (c == 3) {          /* Ctrl-C: abandon the line */
			rk_console_puts("^C\n");
			len = 0;
			break;
		}
		if (c < 32 || c > 126)
			continue;
		if (len + 1 < n) {
			buf[len++] = (char)c;
			rk_console_putc((char)c);
		}
	}
	buf[len] = '\0';
	return (int)len;
}

void rk_console_init(struct boot_info *bi)
{
	spin_lock_init(&console_lock, "console");
	ringbuf_init(&input_ring, input_storage, sizeof(input_storage));
	input_ready = true;

	rk_serial_console_init();
	if (bi && bi->fb.format == RK_FB_RGB)
		rk_fb_console_init(bi);
	else if (bi && bi->fb.format == RK_FB_TEXT)
		rk_vga_console_init();
}
