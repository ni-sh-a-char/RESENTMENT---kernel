/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - formatted output.
 *
 * No floating point conversions. A kernel that must not touch the FPU in
 * interrupt context has no business having %f in its printf, and every value
 * the kernel prints is an integer, a pointer or a string.
 */
#include <rk/printf.h>
#include <rk/string.h>

#define FLAG_LEFT   (1u << 0)
#define FLAG_PLUS   (1u << 1)
#define FLAG_SPACE  (1u << 2)
#define FLAG_ZERO   (1u << 3)
#define FLAG_ALT    (1u << 4)
#define FLAG_UPPER  (1u << 5)

struct out {
	rk_putc_fn fn;
	void      *ctx;
	int        count;
};

static void emit(struct out *o, char c)
{
	o->fn(o->ctx, c);
	o->count++;
}

static void emit_pad(struct out *o, char c, int n)
{
	while (n-- > 0)
		emit(o, c);
}

static void emit_str(struct out *o, const char *s, int len, int width, u32 flags)
{
	int pad = width - len;
	if (!(flags & FLAG_LEFT))
		emit_pad(o, (flags & FLAG_ZERO) ? '0' : ' ', pad);
	for (int i = 0; i < len; i++)
		emit(o, s[i]);
	if (flags & FLAG_LEFT)
		emit_pad(o, ' ', pad);
}

static const char *digits_lo = "0123456789abcdef";
static const char *digits_up = "0123456789ABCDEF";

static void emit_num(struct out *o, u64 value, unsigned base, bool negative,
                     int width, int prec, u32 flags)
{
	char buf[72];
	int  n = 0;
	const char *dg = (flags & FLAG_UPPER) ? digits_up : digits_lo;

	if (value == 0)
		buf[n++] = '0';
	while (value) {
		buf[n++] = dg[value % base];
		value /= base;
	}
	while (n < prec && n < (int)sizeof(buf))
		buf[n++] = '0';

	char prefix[3];
	int  plen = 0;
	if (negative)
		prefix[plen++] = '-';
	else if (flags & FLAG_PLUS)
		prefix[plen++] = '+';
	else if (flags & FLAG_SPACE)
		prefix[plen++] = ' ';
	if (flags & FLAG_ALT) {
		if (base == 16) {
			prefix[plen++] = '0';
			prefix[plen++] = (flags & FLAG_UPPER) ? 'X' : 'x';
		} else if (base == 2) {
			prefix[plen++] = '0';
			prefix[plen++] = 'b';
		}
	}

	int total = n + plen;
	int pad   = width - total;

	/* Zero padding goes after the sign and the 0x, never before it. */
	if (!(flags & FLAG_LEFT) && !(flags & FLAG_ZERO))
		emit_pad(o, ' ', pad);
	for (int i = 0; i < plen; i++)
		emit(o, prefix[i]);
	if (!(flags & FLAG_LEFT) && (flags & FLAG_ZERO))
		emit_pad(o, '0', pad);
	while (n--)
		emit(o, buf[n]);
	if (flags & FLAG_LEFT)
		emit_pad(o, ' ', pad);
}

static void emit_hexdump(struct out *o, const u8 *p, int nbytes)
{
	for (int i = 0; i < nbytes; i++) {
		if (i)
			emit(o, ' ');
		emit(o, digits_lo[p[i] >> 4]);
		emit(o, digits_lo[p[i] & 0xf]);
	}
}

/* Human byte sizes without floating point: one decimal place, computed by
 * scaling the remainder by 10 before the divide. */
static void emit_bytesize(struct out *o, u64 v)
{
	static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
	unsigned u = 0;
	u64 whole = v, frac = 0;

	while (whole >= 1024 && u + 1 < 6) {
		frac  = ((whole % 1024) * 10) / 1024;
		whole /= 1024;
		u++;
	}
	emit_num(o, whole, 10, false, 0, 0, 0);
	if (u) {
		emit(o, '.');
		emit_num(o, frac, 10, false, 0, 0, 0);
	}
	emit(o, ' ');
	for (const char *s = unit[u]; *s; s++)
		emit(o, *s);
}

int rk_vfctprintf(rk_putc_fn out, void *ctx, const char *fmt, va_list ap)
{
	struct out o = { out, ctx, 0 };

	while (*fmt) {
		if (*fmt != '%') {
			emit(&o, *fmt++);
			continue;
		}
		fmt++;

		u32 flags = 0;
		for (;; fmt++) {
			if (*fmt == '-')      flags |= FLAG_LEFT;
			else if (*fmt == '+') flags |= FLAG_PLUS;
			else if (*fmt == ' ') flags |= FLAG_SPACE;
			else if (*fmt == '0') flags |= FLAG_ZERO;
			else if (*fmt == '#') flags |= FLAG_ALT;
			else break;
		}

		int width = 0;
		if (*fmt == '*') {
			int w = va_arg(ap, int);
			if (w < 0) {
				flags |= FLAG_LEFT;
				w = -w;
			}
			width = w;
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}

		int prec = -1;
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			}
		}

		/* length: hh h (none) l ll z t j */
		int len = 4;   /* 0=hh 1=h 4=int 8=long 9=longlong */
		if (*fmt == 'h') {
			fmt++;
			len = 1;
			if (*fmt == 'h') {
				fmt++;
				len = 0;
			}
		} else if (*fmt == 'l') {
			fmt++;
			len = 8;
			if (*fmt == 'l') {
				fmt++;
				len = 9;
			}
		} else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') {
			fmt++;
			len = 8;
		}

		char spec = *fmt++;
		switch (spec) {
		case 'c': {
			char c = (char)va_arg(ap, int);
			emit_str(&o, &c, 1, width, flags & ~FLAG_ZERO);
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			int sl = (int)((prec >= 0) ? strnlen(s, (size_t)prec) : strlen(s));
			emit_str(&o, s, sl, width, flags & ~FLAG_ZERO);
			break;
		}
		case 'd':
		case 'i': {
			s64 v;
			if (len >= 8)      v = va_arg(ap, s64);
			else               v = va_arg(ap, int);
			if (len == 1)      v = (s16)v;
			else if (len == 0) v = (s8)v;
			bool neg = v < 0;
			u64 mag = neg ? (u64)(-(v + 1)) + 1 : (u64)v;  /* safe at INT64_MIN */
			emit_num(&o, mag, 10, neg, width, prec, flags);
			break;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'b': {
			u64 v;
			if (len >= 8)      v = va_arg(ap, u64);
			else               v = va_arg(ap, unsigned);
			if (len == 1)      v = (u16)v;
			else if (len == 0) v = (u8)v;
			unsigned base = spec == 'u' ? 10 : spec == 'o' ? 8 : spec == 'b' ? 2 : 16;
			if (spec == 'X')
				flags |= FLAG_UPPER;
			emit_num(&o, v, base, false, width, prec, flags);
			break;
		}
		case 'p': {
			/* Kernel extensions live behind %p so they cannot collide with
			 * anything a standard format string means. */
			if (*fmt == 'M') {
				fmt++;
				const u8 *p = va_arg(ap, const u8 *);
				emit_hexdump(&o, p, width ? width : 16);
			} else if (*fmt == 'B') {
				fmt++;
				emit_bytesize(&o, (u64)(uintptr_t)va_arg(ap, void *));
			} else {
				void *p = va_arg(ap, void *);
				flags |= FLAG_ALT | FLAG_ZERO;
				emit_num(&o, (u64)(uintptr_t)p, 16, false, width ? width : 18, prec, flags);
			}
			break;
		}
		case '%':
			emit(&o, '%');
			break;
		case '\0':
			return o.count;
		default:
			emit(&o, '%');
			emit(&o, spec);
			break;
		}
	}
	return o.count;
}

int rk_fctprintf(rk_putc_fn out, void *ctx, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = rk_vfctprintf(out, ctx, fmt, ap);
	va_end(ap);
	return n;
}

struct bufsink {
	char  *buf;
	size_t size;
	size_t pos;
};

static void buf_putc(void *ctx, char c)
{
	struct bufsink *b = ctx;
	if (b->pos + 1 < b->size)
		b->buf[b->pos] = c;
	b->pos++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct bufsink b = { buf, size, 0 };
	rk_vfctprintf(buf_putc, &b, fmt, ap);
	if (size)
		buf[b.pos < size ? b.pos : size - 1] = '\0';
	return (int)b.pos;   /* what it would have taken, like C99 */
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
