/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - console.
 *
 * A console is any sink that can take characters early in boot. Several can be
 * registered at once (serial plus framebuffer is the normal case) so a machine
 * with no display still produces a full boot log over a UART.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/compiler.h>
#include <rk/printf.h>

struct boot_info;

struct rk_console {
	char   name[16];
	void (*putc)(struct rk_console *c, char ch);
	void (*write)(struct rk_console *c, const char *s, size_t n);
	void (*clear)(struct rk_console *c);
	void (*set_color)(struct rk_console *c, u8 fg, u8 bg);
	void (*flush)(struct rk_console *c);
	/* Poll for one waiting character, or -1. Optional, but a console that
	 * provides it works even when interrupt routing does not - which is the
	 * difference between a usable machine and a silent one on hardware whose
	 * interrupt controller is described differently than expected. */
	int  (*getc)(struct rk_console *c);
	void  *priv;
	u32    width, height;
	bool   enabled;
	struct list_head link;
};

/* 16-colour palette shared by the text and framebuffer backends. */
enum {
	RK_COLOR_BLACK = 0, RK_COLOR_BLUE, RK_COLOR_GREEN, RK_COLOR_CYAN,
	RK_COLOR_RED, RK_COLOR_MAGENTA, RK_COLOR_BROWN, RK_COLOR_LIGHT_GRAY,
	RK_COLOR_DARK_GRAY, RK_COLOR_LIGHT_BLUE, RK_COLOR_LIGHT_GREEN,
	RK_COLOR_LIGHT_CYAN, RK_COLOR_LIGHT_RED, RK_COLOR_PINK,
	RK_COLOR_YELLOW, RK_COLOR_WHITE
};

void rk_console_init(struct boot_info *bi);
int  rk_console_register(struct rk_console *c);
void rk_console_putc(char c);
void rk_console_write(const char *s, size_t n);
void rk_console_puts(const char *s);
void rk_console_clear(void);
void rk_console_set_color(u8 fg, u8 bg);
int  rk_printf(const char *fmt, ...) __printf(1, 2);

/* Backends. Each is a no-op when the machine does not have that hardware. */
void rk_vga_console_init(void);
void rk_fb_console_init(struct boot_info *bi);
void rk_serial_console_init(void);
void rk_serial_early_init(void);
void rk_serial_putc(char c);

/* Keyboard / line input. Returns bytes read, blocking until a full line. */
int  rk_console_readline(char *buf, size_t n);
int  rk_console_getchar(void);          /* -1 if nothing pending */
void rk_console_push_input(char c);     /* from a keyboard driver, IRQ safe */
