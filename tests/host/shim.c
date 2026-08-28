/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - host test shim.
 *
 * Enough of the kernel environment to run the portable half as an ordinary
 * program: an allocator over a synthetic physical arena, a clock, stubbed
 * scheduling, and logging to stderr.
 *
 * Everything here is a stub EXCEPT the memory manager, which is the real
 * kernel buddy allocator and the real slab code running against a malloc'd
 * arena. That is deliberate: an allocator is exactly the kind of code that
 * passes review and then corrupts memory under load, and it is testable
 * without hardware.
 */
#define RK_HOSTED 1

/* The platform headers come first, and the order is load-bearing.
 *
 * <rk/printf.h> maps snprintf onto the kernel's own implementation so that
 * kernel sources compiled for the host keep using it. If <stdio.h> is
 * processed after that macro exists, glibc declares *rk_snprintf* instead of
 * snprintf - complete with the asm redirection and format attributes it
 * attaches to its own - and every call to rk_snprintf then lands in glibc.
 * The kernel's %pB extension is not glibc's, so it prints a pointer and a
 * stray letter, and only on glibc: the failure does not reproduce under a
 * toolchain whose libc declares snprintf plainly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rk/types.h>
#include <rk/arch.h>
#include <rk/boot.h>
#include <rk/mm.h>
#include <rk/log.h>
#include <rk/sched.h>
#include <rk/sync.h>
#include <rk/time.h>
#include <rk/irq.h>
#include <rk/console.h>
#include <rk/graph.h>
#include <rk/panic.h>
#include <rk/errno.h>
#include <rk/printf.h>

/* ------------------------------------------------------ synthetic memory */

#define ARENA_BYTES (64u << 20)

static u8 *arena;
struct boot_info rk_boot_info;

vaddr_t arch_phys_to_virt(paddr_t pa) { return (vaddr_t)(arena + pa); }
paddr_t arch_virt_to_phys(vaddr_t va) { return (paddr_t)((u8 *)va - arena); }

void rk_host_arena_init(void)
{
	if (arena)
		return;

	/* The arena must be aligned as strongly as the kernel's direct map, which
	 * is 2 MiB aligned. The slab allocator finds a slab header by masking an
	 * object address down to its order-aligned block, and that only works if
	 * physical zero maps to an equally aligned virtual address. A malloc'd
	 * arena is not, so align it here rather than weakening the allocator. */
	size_t align = 2u << 20;
	u8 *raw = calloc(1, ARENA_BYTES + align);
	if (!raw) {
		fprintf(stderr, "cannot allocate the %u MiB test arena\n", ARENA_BYTES >> 20);
		exit(1);
	}
	arena = (u8 *)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));

	memset(&rk_boot_info, 0, sizeof(rk_boot_info));
	rk_boot_info.magic = RK_BOOT_MAGIC;
	strcpy(rk_boot_info.platform, "host-test");
	strcpy(rk_boot_info.firmware, "none");

	/* One usable region covering the arena, with the first megabyte reserved
	 * the way a real machine reserves low memory. */
	rk_boot_info.mmap[0].base = 0;
	rk_boot_info.mmap[0].len  = 0x100000;
	rk_boot_info.mmap[0].type = RK_MEM_RESERVED;
	rk_boot_info.mmap[1].base = 0x100000;
	rk_boot_info.mmap[1].len  = ARENA_BYTES - 0x100000;
	rk_boot_info.mmap[1].type = RK_MEM_USABLE;
	rk_boot_info.mmap_count = 2;

	/* Pretend the kernel image occupies the first 64 KiB of usable memory. */
	rk_boot_info.kernel_phys_start = 0x100000;
	rk_boot_info.kernel_phys_end   = 0x110000;
}

/* --------------------------------------------------------------- arch */

const char *arch_name(void) { return "host"; }
const char *arch_cpu_model(void) { return "host test harness"; }
u32  arch_cpu_id(void) { return 0; }
u32  arch_cpu_count(void) { return 1; }
u64  arch_cpu_features(void) { return RK_FEAT_FPU | RK_FEAT_SIMD; }

void arch_halt(void) { exit(1); }
void arch_idle(void) { }
void arch_cpu_relax(void) { }
void arch_reboot(void) { exit(0); }
void arch_poweroff(void) { exit(0); }

void arch_irq_enable(void) { }
void arch_irq_disable(void) { }
bool arch_irq_enabled(void) { return true; }
unsigned long arch_irq_save(void) { return 0; }
void arch_irq_restore(unsigned long f) { (void)f; }

u64 arch_cycles(void) { return (u64)clock(); }
u64 arch_cycles_per_sec(void) { return CLOCKS_PER_SEC; }

u64 arch_time_ns(void)
{
	/* Monotonic and strictly increasing, which some kernel code relies on. */
	static u64 counter;
	return ++counter * 1000;
}

s64 arch_wallclock_unix(void) { return (s64)time(NULL); }
void arch_timer_init(u32 hz) { (void)hz; }
void arch_timer_oneshot(u64 ns) { (void)ns; }

pgtable_t arch_pgtable_kernel(void) { return NULL; }
pgtable_t arch_pgtable_create(void) { return NULL; }
void arch_pgtable_destroy(pgtable_t p) { (void)p; }
void arch_pgtable_switch(pgtable_t p) { (void)p; }
int  arch_map(pgtable_t p, vaddr_t v, paddr_t a, size_t l, u32 f)
{ (void)p; (void)v; (void)a; (void)l; (void)f; return RK_OK; }
int  arch_unmap(pgtable_t p, vaddr_t v, size_t l) { (void)p; (void)v; (void)l; return RK_OK; }
int  arch_protect(pgtable_t p, vaddr_t v, size_t l, u32 f)
{ (void)p; (void)v; (void)l; (void)f; return RK_OK; }
bool arch_translate(pgtable_t p, vaddr_t v, paddr_t *pa, u32 *pr)
{ (void)p; (void)v; (void)pa; (void)pr; return false; }
void arch_tlb_flush_page(vaddr_t v) { (void)v; }
void arch_tlb_flush_all(void) { }

void arch_thread_init(struct thread *t, void (*e)(void *), void *a, vaddr_t s)
{ (void)t; (void)e; (void)a; (void)s; }
void arch_context_switch(void **s, void *l) { (void)s; (void)l; }
void arch_thread_free(struct thread *t) { (void)t; }
struct thread *arch_current_thread(void) { return NULL; }
void arch_set_current_thread(struct thread *t) { (void)t; }
void arch_fpu_save(struct thread *t) { (void)t; }
void arch_fpu_restore(struct thread *t) { (void)t; }
size_t arch_fpu_state_size(void) { return 512; }
int  arch_enter_user(vaddr_t e, vaddr_t s, void *a)
{ (void)e; (void)s; (void)a; return RK_ENOSYS; }
size_t arch_hw_random(void *b, size_t n) { (void)b; (void)n; return 0; }
int  arch_smp_start_secondaries(void) { return 0; }
void arch_smp_send_ipi(u32 c, u32 v) { (void)c; (void)v; }
void arch_smp_broadcast_ipi(u32 v) { (void)v; }
void arch_early_init(struct boot_info *b) { (void)b; }
void arch_init(struct boot_info *b) { (void)b; }
void arch_late_init(void) { }

void rk_fpu_begin(void) { }
void rk_fpu_end(void) { }

void rk_irq_mask_arch(u32 irq, bool m) { (void)irq; (void)m; }
void rk_irq_eoi_arch(u32 v) { (void)v; }
bool rk_in_irq(void) { return false; }

/* ------------------------------------------------------------- console */

static bool console_quiet = true;

void rk_host_console_verbose(bool on) { console_quiet = !on; }

void rk_console_putc(char c) { if (!console_quiet) fputc(c, stderr); }
void rk_console_write(const char *s, size_t n)
{
	if (!console_quiet)
		fwrite(s, 1, n, stderr);
}
void rk_console_puts(const char *s) { rk_console_write(s, strlen(s)); }
void rk_console_clear(void) { }
void rk_console_set_color(u8 f, u8 b) { (void)f; (void)b; }
void rk_console_push_input(char c) { (void)c; }
int  rk_console_getchar(void) { return -1; }
int  rk_console_readline(char *b, size_t n) { (void)n; b[0] = '\0'; return 0; }
int  rk_console_register(struct rk_console *c) { (void)c; return 0; }
void rk_console_init(struct boot_info *b) { (void)b; }
void rk_vga_console_init(void) { }
void rk_fb_console_init(struct boot_info *b) { (void)b; }
void rk_serial_console_init(void) { }
void rk_serial_early_init(void) { }
void rk_serial_putc(char c) { (void)c; }

int rk_printf(const char *fmt, ...)
{
	if (console_quiet)
		return 0;
	va_list ap;
	va_start(ap, fmt);
	char buf[1024];
	int n = rk_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stderr);
	return n;
}

/* ------------------------------------------------------------------ log */

static enum rk_loglevel host_level = RK_LOG_WARN;

void rk_log_init(void) { }
void rk_log_set_level(enum rk_loglevel l) { host_level = l; }
enum rk_loglevel rk_log_level(void) { return host_level; }
const char *rk_loglevel_name(enum rk_loglevel l) { (void)l; return "log"; }
u64 rk_log_records(void) { return 0; }
size_t rk_log_read(char *b, size_t n, size_t *c) { (void)b; (void)n; (void)c; return 0; }

void rk_log(enum rk_loglevel lvl, const char *subsys, const char *fmt, ...)
{
	if (lvl > host_level)
		return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	rk_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "  [%s] %s\n", subsys, buf);
}

void rk_panic(const char *file, int line, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	rk_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "\nKERNEL PANIC at %s:%d: %s\n", file, line, buf);
	abort();
}

/* ------------------------------------------------------------ scheduling */

bool sched_active(void) { return false; }
void sched_yield(void) { }
void sched_block(void) { }
void sched_wake(struct thread *t) { (void)t; }
void sched_sleep_ns(u64 n) { (void)n; }
void sched_sleep_ms(u64 m) { (void)m; }
void sched_preempt_disable(void) { }
void sched_preempt_enable(void) { }
struct thread *thread_current(void) { return NULL; }
void thread_exit(int c) { exit(c); }

static struct task host_task;
struct task *task_current(void) { return &host_task; }
struct task *task_kernel(void) { return &host_task; }
void task_exit(struct task *t, int c) { (void)t; (void)c; }

void waitq_init(struct waitqueue *w, const char *n) { (void)w; (void)n; }
void waitq_wait(struct waitqueue *w) { (void)w; }
int  waitq_wait_timeout(struct waitqueue *w, u64 n) { (void)w; (void)n; return RK_ETIMEDOUT; }
void waitq_wake_one(struct waitqueue *w) { (void)w; }
void waitq_wake_all(struct waitqueue *w) { (void)w; }

void mutex_init(struct mutex *m, const char *n) { (void)m; (void)n; }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
bool mutex_trylock(struct mutex *m) { (void)m; return true; }

/* -------------------------------------------------------------- timing */

void rk_time_init(void) { }
void rk_udelay(u64 us) { (void)us; }
void rk_mdelay(u64 ms) { (void)ms; }

/* The wall clock is settable so a test can put the machine at a chosen second
 * and check that Kaalka produces the angles that second implies. */
static s64 host_unix = 1735689600;   /* 2025-01-01T00:00:00Z */

void rk_host_set_time(s64 t) { host_unix = t; }
s64  rk_unix_time(void) { return host_unix; }
void rk_set_unix_time(s64 s) { host_unix = s; }
u64  rk_time_ns(void) { return arch_time_ns(); }
u64  rk_time_ms(void) { return rk_time_ns() / 1000000; }
u64  rk_uptime_sec(void) { return rk_time_ns() / 1000000000ull; }
void rk_walltime(struct rk_walltime *o) { o->unix_sec = host_unix; o->nsec = 0; }
