/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - formatted output.
 *
 * Supports: %c %s %d %i %u %x %X %o %b %p %%, length modifiers l/ll/z/h/hh,
 * flags -+0 space #, width, precision, and the kernel extensions:
 *   %pM  - hex dump a buffer; the width field gives the byte count
 *   %pB  - human-readable byte size, e.g. 4.0 KiB
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(d, s)      __builtin_va_copy(d, s)

/* On a hosted test build these would collide with libc, so the kernel's own
 * implementations are renamed and every in-kernel call site follows them.
 * That way the tests exercise this code rather than the host's. */
/* Kernel sources compiled for the host keep calling snprintf and keep getting
 * the kernel's, extensions included. The undef matters: glibc defines snprintf
 * as a fortifying macro, and redefining one macro over another is a warning
 * that some builds promote to an error.
 *
 * Include the platform headers BEFORE this one. Processed the other way round,
 * glibc's own declaration of snprintf is macro-expanded into a declaration of
 * rk_snprintf, asm redirection and all, and every rk_snprintf call in the
 * program silently becomes a glibc call. */
#ifdef RK_HOSTED
#undef snprintf
#undef vsnprintf
#define snprintf  rk_snprintf
#define vsnprintf rk_vsnprintf
#endif

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);

/* Emit through a caller-supplied sink, so formatting never needs a temporary
 * buffer sized for the whole line. The console and log layers both use this. */
/* %pB consumes one pointer-sized argument, exactly like %p, so the compiler
 * format check stays useful. Wrap the value in RK_BYTES at the call site. */
#define RK_BYTES(v) ((void *)(uintptr_t)(u64)(v))

typedef void (*rk_putc_fn)(void *ctx, char c);
int rk_vfctprintf(rk_putc_fn out, void *ctx, const char *fmt, va_list ap);
int rk_fctprintf(rk_putc_fn out, void *ctx, const char *fmt, ...) __printf(3, 4);
