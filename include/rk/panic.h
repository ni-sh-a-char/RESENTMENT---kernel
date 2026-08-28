/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - fatal error handling.
 */
#pragma once

#include <rk/compiler.h>

__noreturn void rk_panic(const char *file, int line, const char *fmt, ...) __printf(3, 4);

#define panic(...) rk_panic(__FILE__, __LINE__, __VA_ARGS__)

#define RK_ASSERT(cond)                                   \
	do {                                                  \
		if (unlikely(!(cond)))                            \
			panic("assertion failed: %s", #cond);         \
	} while (0)

#define RK_ASSERT_MSG(cond, ...)                          \
	do {                                                  \
		if (unlikely(!(cond)))                            \
			panic(__VA_ARGS__);                           \
	} while (0)

#define RK_BUG()         panic("BUG")
#define RK_UNREACHABLE() panic("unreachable")
