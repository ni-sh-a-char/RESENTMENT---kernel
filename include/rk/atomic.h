/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - atomics.
 *
 * Thin wrapper over the compiler's C11 atomic builtins so the kernel does not
 * need <stdatomic.h> (which is hosted-only on some toolchains) and so memory
 * ordering is explicit and greppable.
 */
#pragma once

#include <rk/types.h>

typedef struct { volatile u32 v; } atomic32_t;
typedef struct { volatile u64 v; } atomic64_t;

#define ATOMIC_INIT(x) { (x) }

#define RK_RELAXED __ATOMIC_RELAXED
#define RK_ACQUIRE __ATOMIC_ACQUIRE
#define RK_RELEASE __ATOMIC_RELEASE
#define RK_ACQ_REL __ATOMIC_ACQ_REL
#define RK_SEQ_CST __ATOMIC_SEQ_CST

static inline u32 atomic32_load(const atomic32_t *a)      { return __atomic_load_n(&a->v, RK_ACQUIRE); }
static inline void atomic32_store(atomic32_t *a, u32 x)   { __atomic_store_n(&a->v, x, RK_RELEASE); }
static inline u32 atomic32_add(atomic32_t *a, u32 x)      { return __atomic_add_fetch(&a->v, x, RK_ACQ_REL); }
static inline u32 atomic32_sub(atomic32_t *a, u32 x)      { return __atomic_sub_fetch(&a->v, x, RK_ACQ_REL); }
static inline u32 atomic32_inc(atomic32_t *a)             { return atomic32_add(a, 1); }
static inline u32 atomic32_dec(atomic32_t *a)             { return atomic32_sub(a, 1); }
static inline u32 atomic32_swap(atomic32_t *a, u32 x)     { return __atomic_exchange_n(&a->v, x, RK_ACQ_REL); }
static inline bool atomic32_cas(atomic32_t *a, u32 *exp, u32 nw)
{
	return __atomic_compare_exchange_n(&a->v, exp, nw, false, RK_ACQ_REL, RK_ACQUIRE);
}

static inline u64 atomic64_load(const atomic64_t *a)      { return __atomic_load_n(&a->v, RK_ACQUIRE); }
static inline void atomic64_store(atomic64_t *a, u64 x)   { __atomic_store_n(&a->v, x, RK_RELEASE); }
static inline u64 atomic64_add(atomic64_t *a, u64 x)      { return __atomic_add_fetch(&a->v, x, RK_ACQ_REL); }
static inline u64 atomic64_sub(atomic64_t *a, u64 x)      { return __atomic_sub_fetch(&a->v, x, RK_ACQ_REL); }
static inline u64 atomic64_inc(atomic64_t *a)             { return atomic64_add(a, 1); }
static inline u64 atomic64_swap(atomic64_t *a, u64 x)     { return __atomic_exchange_n(&a->v, x, RK_ACQ_REL); }
static inline bool atomic64_cas(atomic64_t *a, u64 *exp, u64 nw)
{
	return __atomic_compare_exchange_n(&a->v, exp, nw, false, RK_ACQ_REL, RK_ACQUIRE);
}

#define smp_mb()  __atomic_thread_fence(RK_SEQ_CST)
#define smp_rmb() __atomic_thread_fence(RK_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(RK_RELEASE)
