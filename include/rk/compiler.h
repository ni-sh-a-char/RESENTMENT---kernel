/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - compiler abstraction.
 */
#pragma once

#define RK_STR_(x) #x
#define RK_STR(x)  RK_STR_(x)

#define __packed        __attribute__((packed))
#define __aligned(n)    __attribute__((aligned(n)))
#define __noreturn      __attribute__((noreturn))
#define __unused        __attribute__((unused))
#define __used          __attribute__((used))
#define __must_check    __attribute__((warn_unused_result))
#define __printf(a, b)  __attribute__((format(printf, a, b)))
#define __section(s)    __attribute__((section(s)))
#define __weak          __attribute__((weak))
/* glibc's <sys/cdefs.h> defines this too, identically in effect. Redefining
 * it is a warning on every hosted build of the test harness, so don't. */
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif
#define __noinline      __attribute__((noinline))
#define __fallthrough   __attribute__((fallthrough))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define barrier()   __asm__ __volatile__("" ::: "memory")

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)     ((a) < (b) ? (a) : (b))
#define MAX(a, b)     ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX((v), (lo)), (hi))

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

/* stddef.h may already have it; ours is identical, so do not fight it. */
#ifndef offsetof
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif

/* Compile-time assertion usable at file scope. */
#define RK_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
