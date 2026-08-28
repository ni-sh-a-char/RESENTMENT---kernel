/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - freestanding base types.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* Physical and virtual addresses are distinct types by convention: the kernel
 * runs at a higher-half virtual offset, so mixing them is a real bug class. */
typedef uint64_t paddr_t;
typedef uintptr_t vaddr_t;

/* A byte count that can also carry a negative error code. Freestanding builds
 * have no <sys/types.h>; the host test harness defines RK_HOSTED and brings
 * its own so the two definitions cannot conflict. */
#ifndef RK_HOSTED
typedef s64 ssize_t;
#endif

typedef u64 rk_time_t;  /* nanoseconds since boot */
typedef u64 rk_id_t;    /* generic object id */

#define RK_INVALID_ID ((rk_id_t)0)
