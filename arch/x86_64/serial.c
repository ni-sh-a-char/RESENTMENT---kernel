/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - 16550 UART console.
 *
 * The first thing brought up and the last thing to fail. On a headless board
 * or under an emulator this is the entire user interface, so it is initialised
 * before memory management, before the IDT, before anything that could go
 * wrong silently.
 */
#include <arch/x86.h>
#include <rk/console.h>
#include <rk/ringbuf.h>
#include <rk/irq.h>

#define COM1 0x3F8

#define UART_DATA   0
#define UART_IER    1
#define UART_FCR    2
#define UART_LCR    3
#define UART_MCR    4
#define UART_LSR    5

static bool serial_present;

static bool serial_probe(u16 port)
{
	/* Loopback test: if the scratch pattern does not come back, there is no
	 * UART there and writing to it would hang on a machine with no COM port. */
	arch_outb(port + UART_MCR, 0x1E);
	arch_outb(port + UART_DATA, 0xAE);
	return arch_inb(port + UART_DATA) == 0xAE;
}

void x86_serial_init(void)
{
	arch_outb(COM1 + UART_IER, 0x00);   /* interrupts off while configuring */
	arch_outb(COM1 + UART_LCR, 0x80);   /* DLAB: divisor access */
	arch_outb(COM1 + 0, 0x01);          /* 115200 baud */
	arch_outb(COM1 + 1, 0x00);
	arch_outb(COM1 + UART_LCR, 0x03);   /* 8N1 */
	arch_outb(COM1 + UART_FCR, 0xC7);   /* FIFO on, clear, 14-byte threshold */
	arch_outb(COM1 + UART_MCR, 0x0B);   /* DTR, RTS, OUT2 */

	if (!serial_probe(COM1)) {
		serial_present = false;
		return;
	}
	arch_outb(COM1 + UART_MCR, 0x0F);   /* leave loopback */
	serial_present = true;
}

void rk_serial_putc(char c)
{
	if (!serial_present)
		return;
	/* Bounded spin: a disconnected UART with no CTS must not wedge the boot. */
	for (int i = 0; i < 100000; i++)
		if (arch_inb(COM1 + UART_LSR) & 0x20)
			break;
	arch_outb(COM1 + UART_DATA, (u8)c);
}

static void serial_write(struct rk_console *c, const char *s, size_t n)
{
	(void)c;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\n')
			rk_serial_putc('\r');
		rk_serial_putc(s[i]);
	}
}

static void serial_putc_one(struct rk_console *c, char ch) { serial_write(c, &ch, 1); }

/* ANSI colours, so a terminal shows the same severity shading as the screen. */
static const u8 ansi[16] = { 30, 34, 32, 36, 31, 35, 33, 37, 90, 94, 92, 96, 91, 95, 93, 97 };

static void serial_set_color(struct rk_console *c, u8 fg, u8 bg)
{
	(void)c; (void)bg;
	char buf[12];
	int n = snprintf(buf, sizeof(buf), "\033[%um", ansi[fg & 0xF]);
	for (int i = 0; i < n; i++)
		rk_serial_putc(buf[i]);
}

static void serial_clear(struct rk_console *c)
{
	(void)c;
	const char *seq = "\033[2J\033[H";
	while (*seq)
		rk_serial_putc(*seq++);
}

static enum rk_irq_result serial_irq(u32 irq, void *dev)
{
	(void)irq; (void)dev;
	bool any = false;
	while (arch_inb(COM1 + UART_LSR) & 0x01) {
		rk_console_push_input((char)arch_inb(COM1 + UART_DATA));
		any = true;
	}
	return any ? RK_IRQ_HANDLED : RK_IRQ_NONE;
}

/* Polled receive. The interrupt path fills the input ring when it works; this
 * is what makes the console usable when it does not, which on a machine whose
 * interrupt routing is described by firmware we could not read is the
 * difference between a shell and a blank screen. */
static int serial_getc(struct rk_console *c)
{
	(void)c;
	if (!serial_present || !(arch_inb(COM1 + UART_LSR) & 0x01))
		return -1;
	return (int)arch_inb(COM1 + UART_DATA);
}

static struct rk_console serial_console = {
	.name      = "ttyS0",
	.putc      = serial_putc_one,
	.write     = serial_write,
	.clear     = serial_clear,
	.set_color = serial_set_color,
	.getc      = serial_getc,
	.width     = 80,
	.height    = 25,
};

void rk_serial_console_init(void)
{
	if (!serial_present)
		x86_serial_init();
	if (serial_present)
		rk_console_register(&serial_console);
}

void rk_serial_early_init(void) { x86_serial_init(); }

/* Called once interrupts work, so the serial line becomes an input device and
 * not just a log sink. */
void x86_serial_irq_init(void)
{
	if (!serial_present)
		return;
	arch_outb(COM1 + UART_IER, 0x01);   /* receive data available */
	rk_irq_request(4, serial_irq, NULL, NULL, "ttyS0");
	rk_irq_unmask(4);
}
