/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - 128-bit division helpers.
 *
 * The compiler lowers a 128-bit divide into a call to one of these, and a
 * freestanding kernel has no compiler runtime library to provide them. They
 * are needed in two places that matter: the fixed-point divide at the heart of
 * Kaalka, and the cycles-to-nanoseconds conversion in the clock, which
 * overflows 64 bits after a few seconds of uptime on a fast machine.
 *
 * Shift-and-subtract long division. 128 iterations worst case, which sounds
 * expensive until you notice these are called once per fixed-point divide and
 * once per timestamp, not in any inner loop.
 */
#include <rk/types.h>

typedef unsigned __int128 u128;
typedef signed   __int128 s128;

u128 rk_udivmod128(u128 n, u128 d, u128 *rem);

u128 rk_udivmod128(u128 n, u128 d, u128 *rem)
{
	if (d == 0) {
		/* The hardware would trap; there is no sensible value, so return the
		 * saturating one and let the caller's own guard catch it. Every
		 * caller in this kernel checks for a zero divisor first. */
		if (rem)
			*rem = 0;
		return ~(u128)0;
	}

	/* Fast path: both operands fit in 64 bits, which is the common case and
	 * lets the CPU do the work. */
	if (!(n >> 64) && !(d >> 64)) {
		u64 a = (u64)n, b = (u64)d;
		if (rem)
			*rem = a % b;
		return a / b;
	}

	u128 quotient = 0;
	u128 remainder = 0;

	for (int i = 127; i >= 0; i--) {
		remainder = (remainder << 1) | ((n >> i) & 1);
		if (remainder >= d) {
			remainder -= d;
			quotient |= (u128)1 << i;
		}
	}
	if (rem)
		*rem = remainder;
	return quotient;
}

/* The compiler-facing names. A hosted test build already has these from the
 * compiler runtime, so only the freestanding build defines them; the core
 * above stays testable either way. */
#ifndef RK_HOSTED

u128 __udivmodti4(u128 n, u128 d, u128 *rem);
u128 __udivti3(u128 n, u128 d);
u128 __umodti3(u128 n, u128 d);
s128 __divti3(s128 n, s128 d);
s128 __modti3(s128 n, s128 d);

u128 __udivmodti4(u128 n, u128 d, u128 *rem) { return rk_udivmod128(n, d, rem); }
u128 __udivti3(u128 n, u128 d) { return rk_udivmod128(n, d, NULL); }

u128 __umodti3(u128 n, u128 d)
{
	u128 r;
	rk_udivmod128(n, d, &r);
	return r;
}

s128 __divti3(s128 n, s128 d)
{
	int negate = 0;
	if (n < 0) { n = -n; negate ^= 1; }
	if (d < 0) { d = -d; negate ^= 1; }
	u128 q = rk_udivmod128((u128)n, (u128)d, NULL);
	return negate ? -(s128)q : (s128)q;
}

s128 __modti3(s128 n, s128 d)
{
	int negate = (n < 0);
	if (n < 0) n = -n;
	if (d < 0) d = -d;
	u128 r;
	rk_udivmod128((u128)n, (u128)d, &r);
	return negate ? -(s128)r : (s128)r;
}

#endif /* !RK_HOSTED */
