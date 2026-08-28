/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - single producer, single consumer ring buffer.
 *
 * Lock free so a driver interrupt can hand bytes to a thread without ever
 * taking a lock the thread might hold. Capacity must be a power of two, which
 * turns the wrap into a mask instead of a division.
 */
#pragma once

#include <rk/types.h>
#include <rk/atomic.h>
#include <rk/compiler.h>
#include <rk/string.h>

struct ringbuf {
	u8        *buf;
	u32        mask;
	atomic32_t head;   /* written by the producer */
	atomic32_t tail;   /* written by the consumer */
	u64        dropped;
};

static inline void ringbuf_init(struct ringbuf *r, u8 *storage, u32 pow2_size)
{
	r->buf = storage;
	r->mask = pow2_size - 1;
	atomic32_store(&r->head, 0);
	atomic32_store(&r->tail, 0);
	r->dropped = 0;
}

static inline u32 ringbuf_used(const struct ringbuf *r)
{
	return atomic32_load(&r->head) - atomic32_load(&r->tail);
}

static inline u32 ringbuf_space(const struct ringbuf *r)
{
	return (r->mask + 1) - ringbuf_used(r);
}

static inline bool ringbuf_empty(const struct ringbuf *r) { return ringbuf_used(r) == 0; }

static inline bool ringbuf_put(struct ringbuf *r, u8 c)
{
	u32 head = atomic32_load(&r->head);
	if (head - atomic32_load(&r->tail) > r->mask) {
		r->dropped++;
		return false;
	}
	r->buf[head & r->mask] = c;
	atomic32_store(&r->head, head + 1);
	return true;
}

static inline bool ringbuf_get(struct ringbuf *r, u8 *out)
{
	u32 tail = atomic32_load(&r->tail);
	if (tail == atomic32_load(&r->head))
		return false;
	*out = r->buf[tail & r->mask];
	atomic32_store(&r->tail, tail + 1);
	return true;
}

static inline u32 ringbuf_write(struct ringbuf *r, const u8 *src, u32 n)
{
	u32 i = 0;
	while (i < n && ringbuf_put(r, src[i]))
		i++;
	return i;
}

static inline u32 ringbuf_read(struct ringbuf *r, u8 *dst, u32 n)
{
	u32 i = 0;
	while (i < n && ringbuf_get(r, &dst[i]))
		i++;
	return i;
}
