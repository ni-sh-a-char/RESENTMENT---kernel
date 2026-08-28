/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - freestanding string/memory routines.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>

/* The host test harness links against a real libc, so the standard routines
 * come from there and only the kernel-specific ones are ours. Freestanding
 * builds get the whole set from kernel/lib/string.c. */
#ifdef RK_HOSTED
#include <string.h>
#else

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);

#endif /* RK_HOSTED */

/* Truncating, always-NUL-terminating copies. They return the length they tried
 * to create, so truncation is detectable (BSD strlcpy/strlcat semantics). */
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

/* Constant-time comparison for secrets: never short-circuits. */
bool   rk_ct_eq(const void *a, const void *b, size_t n);
void   rk_secure_zero(void *p, size_t n);

u64    rk_strtou64(const char *s, const char **end, int base);
s64    rk_strtos64(const char *s, const char **end, int base);
