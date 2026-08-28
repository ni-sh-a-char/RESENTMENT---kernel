/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - freestanding string and memory routines.
 *
 * These are also what the compiler lowers memcpy/memset calls to, so they must
 * exist even when nothing calls them by name.
 */
#include <rk/string.h>
#include <rk/compiler.h>

/* The standard routines are skipped on a hosted build, where libc already
 * provides them and defining them again would collide at link time. The
 * kernel-specific ones below are always compiled, and are what the host tests
 * actually exercise. */
#ifndef RK_HOSTED

void *memset(void *dst, int c, size_t n)
{
	u8 *d = dst;
	u8  v = (u8)c;

	/* Word-at-a-time for the aligned middle: memset shows up in page zeroing
	 * and tensor fills, where a byte loop is genuinely the bottleneck. */
	while (n && ((uintptr_t)d & 7)) {
		*d++ = v;
		n--;
	}
	if (n >= 8) {
		u64 w = 0x0101010101010101ull * v;
		u64 *dw = (u64 *)d;
		size_t words = n / 8;
		for (size_t i = 0; i < words; i++)
			dw[i] = w;
		d += words * 8;
		n -= words * 8;
	}
	while (n--)
		*d++ = v;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	u8       *d = dst;
	const u8 *s = src;

	if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
		while (n && ((uintptr_t)d & 7)) {
			*d++ = *s++;
			n--;
		}
		size_t words = n / 8;
		u64 *dw = (u64 *)d;
		const u64 *sw = (const u64 *)s;
		for (size_t i = 0; i < words; i++)
			dw[i] = sw[i];
		d += words * 8;
		s += words * 8;
		n -= words * 8;
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	u8       *d = dst;
	const u8 *s = src;

	if (d == s || n == 0)
		return dst;
	if (d < s)
		return memcpy(dst, src, n);

	/* Overlapping and moving up: copy backwards. */
	d += n;
	s += n;
	while (n--)
		*--d = *--s;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const u8 *x = a, *y = b;
	for (size_t i = 0; i < n; i++)
		if (x[i] != y[i])
			return (int)x[i] - (int)y[i];
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const u8 *p = s;
	for (size_t i = 0; i < n; i++)
		if (p[i] == (u8)c)
			return (void *)(p + i);
	return NULL;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
	size_t n = 0;
	while (n < max && s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	if (n == 0)
		return 0;
	return (int)(u8)*a - (int)(u8)*b;
}

static inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int strcasecmp(const char *a, const char *b)
{
	while (*a && lower(*a) == lower(*b)) {
		a++;
		b++;
	}
	return (int)(u8)lower(*a) - (int)(u8)lower(*b);
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *last = NULL;
	for (;; s++) {
		if (*s == (char)c)
			last = s;
		if (!*s)
			return (char *)last;
	}
}

char *strstr(const char *h, const char *n)
{
	if (!*n)
		return (char *)h;
	size_t nl = strlen(n);
	for (; *h; h++)
		if (*h == *n && strncmp(h, n, nl) == 0)
			return (char *)h;
	return NULL;
}

#endif /* !RK_HOSTED */

size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t srclen = strlen(src);
	if (size) {
		size_t copy = srclen < size - 1 ? srclen : size - 1;
		memcpy(dst, src, copy);
		dst[copy] = '\0';
	}
	return srclen;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
	size_t dlen = strnlen(dst, size);
	if (dlen == size)
		return size + strlen(src);
	return dlen + strlcpy(dst + dlen, src, size - dlen);
}

/* Constant time: the loop never exits early and the result depends only on the
 * accumulated difference, so timing leaks nothing about where bytes diverged. */
bool rk_ct_eq(const void *a, const void *b, size_t n)
{
	const u8 *x = a, *y = b;
	u8 diff = 0;
	for (size_t i = 0; i < n; i++)
		diff |= (u8)(x[i] ^ y[i]);
	return diff == 0;
}

/* A volatile pointer so the compiler cannot decide the store is dead. Wiping
 * key material is only useful if it actually happens. */
void rk_secure_zero(void *p, size_t n)
{
	volatile u8 *v = p;
	while (n--)
		*v++ = 0;
	barrier();
}

static int digit_of(char c, int base)
{
	int d;
	if (c >= '0' && c <= '9')
		d = c - '0';
	else if (c >= 'a' && c <= 'z')
		d = c - 'a' + 10;
	else if (c >= 'A' && c <= 'Z')
		d = c - 'A' + 10;
	else
		return -1;
	return d < base ? d : -1;
}

u64 rk_strtou64(const char *s, const char **end, int base)
{
	u64 v = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	if (base == 0) {
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			s += 2;
		} else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
			base = 2;
			s += 2;
		} else if (s[0] == '0' && s[1]) {
			base = 8;
			s++;
		} else {
			base = 10;
		}
	} else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}

	for (;;) {
		int d = digit_of(*s, base);
		if (d < 0)
			break;
		v = v * (u64)base + (u64)d;
		s++;
	}
	if (end)
		*end = s;
	return v;
}

s64 rk_strtos64(const char *s, const char **end, int base)
{
	bool neg = false;

	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '-') {
		neg = true;
		s++;
	} else if (*s == '+') {
		s++;
	}
	u64 v = rk_strtou64(s, end, base);
	return neg ? -(s64)v : (s64)v;
}
