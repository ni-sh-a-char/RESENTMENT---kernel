/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT - the userspace side of the system call ABI.
 *
 * This is the whole of userspace's dependency on the kernel: three inline
 * assembly stubs and a handful of numbers. There is no libc here and there
 * does not need to be one - a program that can make a system call and read its
 * own arguments is a complete program.
 *
 * The register conventions match include/rk/syscall.h. They are the natural
 * ones for each architecture, which is why they differ:
 *
 *   x86_64   syscall, number in rax, args in rdi rsi rdx r10 r8 r9, ret rax
 *            rcx and r11 are clobbered by the instruction itself
 *   aarch64  svc #0,  number in x8,  args in x0..x5,                ret x0
 *   riscv64  ecall,   number in a7,  args in a0..a5,                ret a0
 */
#ifndef RKSYS_H
#define RKSYS_H

typedef unsigned long      u64_t;
typedef long               s64_t;
typedef unsigned int       u32_t;
typedef unsigned char      u8_t;
typedef unsigned long      size_t_;

/* Only the calls this program actually makes. The full table is in
 * include/rk/syscall.h. */
#define SYS_EXIT        0
#define SYS_YIELD       4
#define SYS_GETPID      9
#define SYS_GETTID      10
#define SYS_TIME_NS     160
#define SYS_WALLCLOCK   161
#define SYS_RANDOM      162
#define SYS_SYSINFO     321
#define SYS_DEBUG_PUTS  322

#if defined(__x86_64__)

static inline long rk_syscall(long nr, long a0, long a1, long a2)
{
	long ret;
	register long r10 __asm__("r10") = a2;
	__asm__ __volatile__("syscall"
	                     : "=a"(ret)
	                     : "a"(nr), "D"(a0), "S"(a1), "r"(r10)
	                     : "rcx", "r11", "memory");
	return ret;
}

#elif defined(__aarch64__)

static inline long rk_syscall(long nr, long a0, long a1, long a2)
{
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	__asm__ __volatile__("svc #0"
	                     : "+r"(x0)
	                     : "r"(x8), "r"(x1), "r"(x2)
	                     : "memory");
	return x0;
}

#elif defined(__riscv)

static inline long rk_syscall(long nr, long a0, long a1, long a2)
{
	register long a7 __asm__("a7") = nr;
	register long r0 __asm__("a0") = a0;
	register long r1 __asm__("a1") = a1;
	register long r2 __asm__("a2") = a2;
	__asm__ __volatile__("ecall"
	                     : "+r"(r0)
	                     : "r"(a7), "r"(r1), "r"(r2)
	                     : "memory");
	return r0;
}

#else
#error "no system call convention for this architecture"
#endif

static inline long rk_sys0(long nr)          { return rk_syscall(nr, 0, 0, 0); }
static inline long rk_sys1(long nr, long a)  { return rk_syscall(nr, a, 0, 0); }

static inline void rk_exit(int code)
{
	rk_sys1(SYS_EXIT, code);
	for (;;) { }              /* the kernel does not return from exit */
}

static inline void rk_puts(const char *s) { rk_sys1(SYS_DEBUG_PUTS, (long)s); }
static inline long rk_getpid(void)        { return rk_sys0(SYS_GETPID); }
static inline long rk_time_ns(void)       { return rk_sys0(SYS_TIME_NS); }
static inline long rk_wallclock(void)     { return rk_sys0(SYS_WALLCLOCK); }
static inline long rk_yield(void)         { return rk_sys0(SYS_YIELD); }

static inline long rk_random(void *buf, unsigned long len)
{
	return rk_syscall(SYS_RANDOM, (long)buf, (long)len, 0);
}

static inline long rk_sysinfo(void *out, unsigned long len)
{
	return rk_syscall(SYS_SYSINFO, (long)out, (long)len, 0);
}

/* --------------------------------------------------------------- helpers */

/* No libc, so the two string functions this program needs live here. Both are
 * bounded: an unbounded one in a program with no memory protection below it
 * would be a poor advertisement for a capability kernel. */
static inline unsigned long rk_strlen(const char *s)
{
	unsigned long n = 0;
	while (s[n])
		n++;
	return n;
}

/* Writes `v` in base 10 into `buf`, which must hold at least 21 bytes, and
 * returns it. */
static inline char *rk_utoa(unsigned long v, char *buf)
{
	char tmp[21];
	int i = 0;
	if (!v)
		tmp[i++] = '0';
	while (v) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	int j = 0;
	while (i)
		buf[j++] = tmp[--i];
	buf[j] = '\0';
	return buf;
}

#endif /* RKSYS_H */
