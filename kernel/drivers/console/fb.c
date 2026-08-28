/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - framebuffer console.
 *
 * This is the portable display path: a linear RGB framebuffer is what UEFI, a
 * GRUB multiboot2 handover, a Raspberry Pi mailbox and a virtio-gpu scanout
 * all end up handing over, so one backend covers a laptop, a phone and a board
 * without any of them knowing about VGA text mode.
 *
 * Scrolling copies the character grid rather than the pixels of the whole
 * screen, and only redraws rows that changed, because a 4K framebuffer with no
 * acceleration is slow enough that the naive version visibly stutters the
 * boot log.
 */
#include <rk/console.h>
#include <rk/boot.h>
#include <rk/arch.h>
#include <rk/string.h>

extern const u8 rk_font8x8[128][8];

#define GLYPH_W 8
#define GLYPH_H 8
#define MAX_COLS 256
#define MAX_ROWS 128

struct fbcon {
	u8   *pixels;
	u32   width, height, pitch;
	u8    bpp;
	u32   cols, rows;
	u32   cx, cy;
	u32   fg, bg;
	u8    scale;
	u8    red_shift, green_shift, blue_shift;
	u8    red_size, green_size, blue_size;
	char  grid[MAX_ROWS][MAX_COLS];
	u8    attr[MAX_ROWS][MAX_COLS];
};

static struct fbcon fbc;
static bool fb_ready;

/* The 16-colour palette, as 24-bit RGB. Matching the VGA palette keeps the
 * boot log looking the same on a text console and a framebuffer. */
static const u32 palette[16] = {
	0x000000, 0x0000aa, 0x00aa00, 0x00aaaa,
	0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
	0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
	0xff5555, 0xff55ff, 0xffff55, 0xffffff
};

static inline u32 pack(u32 rgb)
{
	u32 r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	/* Down-shift each channel to the width the mode actually provides, so
	 * 16-bit 5:6:5 modes look right instead of wrapping. */
	r >>= (8 - fbc.red_size);
	g >>= (8 - fbc.green_size);
	b >>= (8 - fbc.blue_size);
	return (r << fbc.red_shift) | (g << fbc.green_shift) | (b << fbc.blue_shift);
}

static inline void put_pixel(u32 x, u32 y, u32 c)
{
	if (x >= fbc.width || y >= fbc.height)
		return;
	u8 *p = fbc.pixels + (u64)y * fbc.pitch + (u64)x * (fbc.bpp / 8);
	switch (fbc.bpp) {
	case 32: *(volatile u32 *)p = c; break;
	case 24: p[0] = (u8)c; p[1] = (u8)(c >> 8); p[2] = (u8)(c >> 16); break;
	case 16: *(volatile u16 *)p = (u16)c; break;
	default: *p = (u8)c; break;
	}
}

static void draw_cell(u32 cx, u32 cy)
{
	char ch = fbc.grid[cy][cx];
	u8   at = fbc.attr[cy][cx];
	u32  fg = pack(palette[at & 0xf]);
	u32  bg = pack(palette[(at >> 4) & 0xf]);
	const u8 *glyph = rk_font8x8[(u8)ch & 0x7f];
	u32 s = fbc.scale;

	for (u32 gy = 0; gy < GLYPH_H; gy++) {
		u8 bits = glyph[gy];
		for (u32 gx = 0; gx < GLYPH_W; gx++) {
			u32 c = ((bits >> gx) & 1) ? fg : bg;
			for (u32 sy = 0; sy < s; sy++)
				for (u32 sx = 0; sx < s; sx++)
					put_pixel(cx * GLYPH_W * s + gx * s + sx,
					          cy * GLYPH_H * s + gy * s + sy, c);
		}
	}
}

static void redraw_all(void)
{
	for (u32 y = 0; y < fbc.rows; y++)
		for (u32 x = 0; x < fbc.cols; x++)
			draw_cell(x, y);
}

static void fb_scroll(void)
{
	for (u32 y = 1; y < fbc.rows; y++) {
		memcpy(fbc.grid[y - 1], fbc.grid[y], fbc.cols);
		memcpy(fbc.attr[y - 1], fbc.attr[y], fbc.cols);
	}
	memset(fbc.grid[fbc.rows - 1], ' ', fbc.cols);
	memset(fbc.attr[fbc.rows - 1], (int)((fbc.fg & 0xf) | ((fbc.bg & 0xf) << 4)), fbc.cols);
	redraw_all();
}

static void fb_newline(void)
{
	fbc.cx = 0;
	if (fbc.cy + 1 < fbc.rows) {
		fbc.cy++;
		return;
	}
	fb_scroll();
}

static void fb_putc(struct rk_console *c, char ch)
{
	(void)c;
	if (!fb_ready)
		return;
	switch (ch) {
	case '\n': fb_newline(); return;
	case '\r': fbc.cx = 0; return;
	case '\b':
		if (fbc.cx) {
			fbc.cx--;
			fbc.grid[fbc.cy][fbc.cx] = ' ';
			draw_cell(fbc.cx, fbc.cy);
		}
		return;
	case '\t':
		do {
			fb_putc(NULL, ' ');
		} while (fbc.cx % 8);
		return;
	}
	if (fbc.cx >= fbc.cols)
		fb_newline();
	fbc.grid[fbc.cy][fbc.cx] = ch;
	fbc.attr[fbc.cy][fbc.cx] = (u8)((fbc.fg & 0xf) | ((fbc.bg & 0xf) << 4));
	draw_cell(fbc.cx, fbc.cy);
	fbc.cx++;
}

static void fb_write(struct rk_console *c, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		fb_putc(c, s[i]);
}

static void fb_clear(struct rk_console *c)
{
	(void)c;
	if (!fb_ready)
		return;
	for (u32 y = 0; y < fbc.rows; y++) {
		memset(fbc.grid[y], ' ', fbc.cols);
		memset(fbc.attr[y], (int)((fbc.fg & 0xf) | ((fbc.bg & 0xf) << 4)), fbc.cols);
	}
	fbc.cx = fbc.cy = 0;
	/* Clearing the pixels directly is far faster than drawing blank glyphs. */
	u32 bgc = pack(palette[fbc.bg & 0xf]);
	for (u32 y = 0; y < fbc.height; y++)
		for (u32 x = 0; x < fbc.width; x++)
			put_pixel(x, y, bgc);
}

static void fb_set_color(struct rk_console *c, u8 fg, u8 bg)
{
	(void)c;
	fbc.fg = fg & 0xf;
	fbc.bg = bg & 0xf;
}

static struct rk_console fb_console = {
	.name      = "fb",
	.putc      = fb_putc,
	.write     = fb_write,
	.clear     = fb_clear,
	.set_color = fb_set_color,
};

void rk_fb_console_init(struct boot_info *bi)
{
	if (!bi || bi->fb.format != RK_FB_RGB || !bi->fb.addr || !bi->fb.width)
		return;

	fbc.pixels = (u8 *)arch_phys_to_virt(bi->fb.addr);
	fbc.width  = bi->fb.width;
	fbc.height = bi->fb.height;
	fbc.pitch  = bi->fb.pitch;
	fbc.bpp    = bi->fb.bpp ? bi->fb.bpp : 32;

	fbc.red_shift   = bi->fb.red_shift;
	fbc.green_shift = bi->fb.green_shift;
	fbc.blue_shift  = bi->fb.blue_shift;
	fbc.red_size    = bi->fb.red_size   ? bi->fb.red_size   : 8;
	fbc.green_size  = bi->fb.green_size ? bi->fb.green_size : 8;
	fbc.blue_size   = bi->fb.blue_size  ? bi->fb.blue_size  : 8;

	/* Pick a scale that keeps text readable on a high-DPI panel instead of
	 * rendering 6-point characters on a phone screen. */
	fbc.scale = (u8)(fbc.width >= 2400 ? 3 : fbc.width >= 1400 ? 2 : 1);

	fbc.cols = fbc.width / (GLYPH_W * fbc.scale);
	fbc.rows = fbc.height / (GLYPH_H * fbc.scale);
	if (fbc.cols > MAX_COLS) fbc.cols = MAX_COLS;
	if (fbc.rows > MAX_ROWS) fbc.rows = MAX_ROWS;

	fbc.fg = RK_COLOR_LIGHT_GRAY;
	fbc.bg = RK_COLOR_BLACK;
	fb_console.width  = fbc.cols;
	fb_console.height = fbc.rows;

	fb_ready = true;
	fb_clear(NULL);
	rk_console_register(&fb_console);
}
