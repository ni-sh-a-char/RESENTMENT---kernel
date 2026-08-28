/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - legacy VGA text console.
 *
 * Only a PC has this. It is kept because it is the one output path that works
 * on a bare PC before any memory management exists, which makes it the last
 * resort when a framebuffer handover has gone wrong.
 *
 * This is the direct descendant of the original RESENTMENT print.c, now behind
 * the console interface so it is one backend among several rather than the
 * only way the kernel can speak.
 */
#include <rk/console.h>
#include <rk/arch.h>
#include <rk/string.h>

#if defined(RK_ARCH_X86_64)

/* The only place outside arch/ that reaches into an architecture header, and
 * only because legacy VGA is x86 by definition: this whole file compiles to
 * nothing anywhere else. */
#include <arch/x86.h>

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_BASE 0xb8000ul

struct vga_cell {
	u8 ch;
	u8 attr;
} __packed;

/* Resolved through the direct map, not used as a raw physical address. The
 * identity mapping that made 0xB8000 work directly is torn down as soon as the
 * kernel is running from its high alias, and a driver still holding a physical
 * pointer at that moment faults inside the panic handler, which then faults
 * again trying to report it. */
static struct vga_cell *vga;
static u32 col, row;
static u8  attr = RK_COLOR_LIGHT_GRAY | (RK_COLOR_BLACK << 4);

static void vga_clear_row(u32 r)
{
	for (u32 c = 0; c < VGA_COLS; c++)
		vga[c + VGA_COLS * r] = (struct vga_cell){ ' ', attr };
}

static void vga_scroll(void)
{
	for (u32 r = 1; r < VGA_ROWS; r++)
		for (u32 c = 0; c < VGA_COLS; c++)
			vga[c + VGA_COLS * (r - 1)] = vga[c + VGA_COLS * r];
	vga_clear_row(VGA_ROWS - 1);
}

static void vga_newline(void)
{
	col = 0;
	if (row + 1 < VGA_ROWS) {
		row++;
		return;
	}
	vga_scroll();
}

/* Move the hardware cursor so a real machine shows where output stopped. */
static void vga_move_cursor(void)
{
	u16 pos = (u16)(row * VGA_COLS + col);
	arch_outb(0x3d4, 0x0f);
	arch_outb(0x3d5, (u8)(pos & 0xff));
	arch_outb(0x3d4, 0x0e);
	arch_outb(0x3d5, (u8)(pos >> 8));
}

static void vga_putc(struct rk_console *c, char ch)
{
	(void)c;
	switch (ch) {
	case '\n':
		vga_newline();
		return;
	case '\r':
		col = 0;
		return;
	case '\t':
		do {
			vga_putc(NULL, ' ');
		} while (col % 8);
		return;
	case '\b':
		if (col)
			col--;
		return;
	}
	if (col >= VGA_COLS)
		vga_newline();
	vga[col + VGA_COLS * row] = (struct vga_cell){ (u8)ch, attr };
	col++;
}

static void vga_write(struct rk_console *c, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		vga_putc(c, s[i]);
	vga_move_cursor();
}

static void vga_do_clear(struct rk_console *c)
{
	(void)c;
	for (u32 r = 0; r < VGA_ROWS; r++)
		vga_clear_row(r);
	col = row = 0;
	vga_move_cursor();
}

static void vga_set_color(struct rk_console *c, u8 fg, u8 bg)
{
	(void)c;
	attr = (u8)((fg & 0xf) | ((bg & 0xf) << 4));
}

static struct rk_console vga_console = {
	.name      = "vga",
	.putc      = vga_putc,
	.write     = vga_write,
	.clear     = vga_do_clear,
	.set_color = vga_set_color,
	.width     = VGA_COLS,
	.height    = VGA_ROWS,
};

void rk_vga_console_init(void)
{
	vga = (struct vga_cell *)arch_phys_to_virt(VGA_BASE);
	vga_do_clear(NULL);
	rk_console_register(&vga_console);
}

#else  /* not a PC */

void rk_vga_console_init(void) { }

#endif
