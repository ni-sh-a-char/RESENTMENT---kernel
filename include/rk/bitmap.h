/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - bitmaps.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>
#include <rk/string.h>

#define BITS_PER_WORD 64u
#define BITMAP_WORDS(n) DIV_ROUND_UP((n), BITS_PER_WORD)
#define DECLARE_BITMAP(name, n) u64 name[BITMAP_WORDS(n)]

static inline void bitmap_zero(u64 *b, size_t nbits) { memset(b, 0, BITMAP_WORDS(nbits) * 8); }
static inline void bitmap_fill(u64 *b, size_t nbits) { memset(b, 0xff, BITMAP_WORDS(nbits) * 8); }
static inline void bitmap_set(u64 *b, size_t i)      { b[i / BITS_PER_WORD] |= 1ull << (i % BITS_PER_WORD); }
static inline void bitmap_clear(u64 *b, size_t i)    { b[i / BITS_PER_WORD] &= ~(1ull << (i % BITS_PER_WORD)); }
static inline bool bitmap_test(const u64 *b, size_t i) { return (b[i / BITS_PER_WORD] >> (i % BITS_PER_WORD)) & 1; }

static inline size_t bitmap_ffs(const u64 *b, size_t nbits)
{
	size_t words = BITMAP_WORDS(nbits);
	for (size_t w = 0; w < words; w++) {
		if (b[w]) {
			size_t bit = w * BITS_PER_WORD + (size_t)__builtin_ctzll(b[w]);
			return bit < nbits ? bit : nbits;
		}
	}
	return nbits;
}

static inline size_t bitmap_ffz(const u64 *b, size_t nbits)
{
	size_t words = BITMAP_WORDS(nbits);
	for (size_t w = 0; w < words; w++) {
		u64 inv = ~b[w];
		if (inv) {
			size_t bit = w * BITS_PER_WORD + (size_t)__builtin_ctzll(inv);
			return bit < nbits ? bit : nbits;
		}
	}
	return nbits;
}

static inline size_t bitmap_weight(const u64 *b, size_t nbits)
{
	size_t words = BITMAP_WORDS(nbits), n = 0;
	for (size_t w = 0; w + 1 < words + 1 && w < words; w++)
		n += (size_t)__builtin_popcountll(b[w]);
	/* Mask off bits past nbits in the final word. */
	size_t rem = nbits % BITS_PER_WORD;
	if (rem && words)
		n -= (size_t)__builtin_popcountll(b[words - 1] & ~((1ull << rem) - 1));
	return n;
}
