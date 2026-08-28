/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - architecture abstraction layer (HAL).
 *
 * This is the entire contract between the portable kernel and a machine.
 * Porting RESENTMENT to a new CPU or board means implementing exactly this
 * header plus a boot stub; nothing above arch/ may include an arch header.
 *
 * Implemented by: arch/x86_64, arch/aarch64, arch/riscv64.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>

/* ---------------------------------------------------------------- identity */

#define RK_MAX_CPUS 256

const char *arch_name(void);            /* "x86_64" */
const char *arch_cpu_model(void);       /* vendor/model string, best effort */
u32   arch_cpu_id(void);                /* logical id of the calling CPU */
u32   arch_cpu_count(void);             /* CPUs brought online */
u64   arch_cpu_features(void);          /* RK_FEAT_* bitmap */

#define RK_FEAT_FPU      (1ull << 0)
#define RK_FEAT_SIMD     (1ull << 1)   /* SSE2 / NEON / RVV */
#define RK_FEAT_SIMD256  (1ull << 2)   /* AVX2 / SVE256 */
#define RK_FEAT_SIMD512  (1ull << 3)   /* AVX512 / SVE512 */
#define RK_FEAT_AES      (1ull << 4)
#define RK_FEAT_SHA      (1ull << 5)
#define RK_FEAT_RDRAND   (1ull << 6)
#define RK_FEAT_INVARTSC (1ull << 7)   /* constant-rate timestamp counter */
#define RK_FEAT_NX       (1ull << 8)
#define RK_FEAT_SMEP     (1ull << 9)
#define RK_FEAT_SMAP     (1ull << 10)
#define RK_FEAT_PCID     (1ull << 11)
#define RK_FEAT_1GPAGE   (1ull << 12)
#define RK_FEAT_X2APIC   (1ull << 13)
#define RK_FEAT_DOTPROD  (1ull << 14)  /* int8 dot product: matters for AI ops */
#define RK_FEAT_FP16     (1ull << 15)
#define RK_FEAT_BF16     (1ull << 16)

/* ------------------------------------------------------------------- setup */

struct boot_info;

void arch_early_init(struct boot_info *bi);  /* serial + minimal console only */
void arch_init(struct boot_info *bi);        /* traps, MMU, IRQ controller, timer */
void arch_late_init(void);                   /* SMP, buses; after mm and sched */

/* --------------------------------------------------------------- execution */

__noreturn void arch_halt(void);
void arch_idle(void);        /* wait for interrupt, low power */
void arch_cpu_relax(void);   /* spin-wait hint (pause / yield / wfe) */
__noreturn void arch_reboot(void);
void arch_poweroff(void);

/* ---------------------------------------------------------------- interrupts */

void arch_irq_enable(void);
void arch_irq_disable(void);
bool arch_irq_enabled(void);
unsigned long arch_irq_save(void);
void arch_irq_restore(unsigned long flags);

/* ---------------------------------------------------------------------- time */

u64 arch_cycles(void);       /* raw cycle counter, monotonic per CPU */
u64 arch_cycles_per_sec(void);
u64 arch_time_ns(void);      /* monotonic nanoseconds since boot */
s64 arch_wallclock_unix(void); /* seconds since epoch from the RTC, 0 if none */

void arch_timer_init(u32 hz);
void arch_timer_oneshot(u64 ns);

/* -------------------------------------------------------------------- memory */

#define RK_PAGE_SHIFT 12u
#define RK_PAGE_SIZE  (1ul << RK_PAGE_SHIFT)
#define RK_PAGE_MASK  (RK_PAGE_SIZE - 1)

/* Page protection bits, portable across all supported MMUs. */
#define RK_PROT_READ    (1u << 0)
#define RK_PROT_WRITE   (1u << 1)
#define RK_PROT_EXEC    (1u << 2)
#define RK_PROT_USER    (1u << 3)
#define RK_PROT_NOCACHE (1u << 4)
#define RK_PROT_WC      (1u << 5)  /* write-combining, for framebuffers */
#define RK_PROT_GLOBAL  (1u << 6)

typedef struct arch_pgtable *pgtable_t;

pgtable_t arch_pgtable_kernel(void);
pgtable_t arch_pgtable_create(void);
void      arch_pgtable_destroy(pgtable_t pt);
void      arch_pgtable_switch(pgtable_t pt);

int  arch_map(pgtable_t pt, vaddr_t va, paddr_t pa, size_t len, u32 prot);
int  arch_unmap(pgtable_t pt, vaddr_t va, size_t len);
int  arch_protect(pgtable_t pt, vaddr_t va, size_t len, u32 prot);
bool arch_translate(pgtable_t pt, vaddr_t va, paddr_t *out_pa, u32 *out_prot);
void arch_tlb_flush_page(vaddr_t va);
void arch_tlb_flush_all(void);

/* The direct map: every byte of physical RAM is visible at a fixed virtual
 * offset, so the kernel can touch any page without a temporary mapping. */
vaddr_t arch_phys_to_virt(paddr_t pa);
paddr_t arch_virt_to_phys(vaddr_t va);

/* ------------------------------------------------------------------ threads */

struct thread;

/* Build an initial stack so that arch_context_switch into it lands at entry. */
void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg,
                      vaddr_t stack_top);
void arch_context_switch(void **save_sp, void *load_sp);
void arch_thread_free(struct thread *t);

/* Per-CPU pointer to the running thread, read without a lock. */
struct thread *arch_current_thread(void);
void arch_set_current_thread(struct thread *t);

/* Lazy FPU/SIMD state. The AI subsystem is the heaviest user, so this is on
 * the fast path, not an afterthought. */
void arch_fpu_save(struct thread *t);
void arch_fpu_restore(struct thread *t);
size_t arch_fpu_state_size(void);

/* Floating point is illegal in kernel code except between these two calls.
 * Most of the kernel is compiled with the FPU and vector registers disabled
 * precisely so the compiler cannot slip a vector instruction into an
 * interrupt path where the register state is not saved. The AI subsystem is
 * the exception: it is compiled with SIMD enabled and must bracket every
 * float-using region, which saves the interrupted thread's state and disables
 * preemption for the duration. Nesting is allowed and counted. */
void rk_fpu_begin(void);
void rk_fpu_end(void);

/* ------------------------------------------------------------- user context */

struct arch_uctx;
int  arch_enter_user(vaddr_t entry, vaddr_t stack, void *arg) __must_check;

/* Make instructions written as data visible to the instruction fetcher.
 *
 * Required after anything that produces code at run time - loading a program
 * image above all. x86 keeps its caches coherent in hardware and this is a
 * no-op there; ARM and RISC-V do not, and skipping it produces a fault on the
 * first instruction of a program that was written correctly, which is a very
 * long way from the cause. */
void arch_sync_icache(vaddr_t va, size_t len);

/* Open a window in which kernel code may touch user pages.
 *
 * Every architecture here can forbid it, and by default does: x86 has SMAP,
 * RISC-V has sstatus.SUM, ARM has PAN. That is a good default - the kernel
 * dereferencing a user pointer by accident is a whole bug class - but the
 * kernel does have a handful of places where touching user memory is the
 * entire point: the bounded copy helpers, and the program loader writing a
 * segment to the address the linker chose.
 *
 * Those places say so, narrowly, rather than the kernel leaving the door open
 * for its whole life. Nesting is allowed; the window closes when the outermost
 * end() runs.
 *
 * Forgetting this on RISC-V does not fail gracefully: the store faults, the
 * fault handler maps a page that was already mapped, returns success, and the
 * instruction retries and faults again, for ever.
 *
 * Preemption is disabled for the duration, and that is not incidental. The
 * permission lives in a per-CPU register - sstatus.SUM, or the SMAP flag in
 * EFLAGS - which no context switch on this kernel saves. A thread preempted
 * inside the window would leave the door open for whatever ran next, and if it
 * were then migrated it would close a window it never opened on the other
 * core. Holding preemption off for a copy is the cheap way to make both
 * impossible.
 *
 * ponytail: preemption held across a whole segment copy. Save the flag per
 * thread in the context switch if a loader ever has to move something big
 * enough to matter. */
void arch_user_access_begin(void);
void arch_user_access_end(void);

/* ------------------------------------------------------------------- random */

/* Hardware entropy if the CPU has it. Returns bytes actually produced. */
size_t arch_hw_random(void *buf, size_t len);

/* ---------------------------------------------------------------------- SMP */

int  arch_smp_start_secondaries(void);
void arch_smp_send_ipi(u32 cpu, u32 vector);
void arch_smp_broadcast_ipi(u32 vector);

#define RK_IPI_RESCHED  0xF0
#define RK_IPI_TLB      0xF1
#define RK_IPI_HALT     0xF2
#define RK_IPI_CALL     0xF3

/* Called by the architecture interrupt path when one of the above arrives. */
void rk_ipi_handle(u32 kind);

/* --------------------------------------------------------------- port / mmio */

static inline u8  mmio_r8 (vaddr_t a) { return *(volatile u8  *)a; }
static inline u16 mmio_r16(vaddr_t a) { return *(volatile u16 *)a; }
static inline u32 mmio_r32(vaddr_t a) { return *(volatile u32 *)a; }
static inline u64 mmio_r64(vaddr_t a) { return *(volatile u64 *)a; }
static inline void mmio_w8 (vaddr_t a, u8  v) { *(volatile u8  *)a = v; }
static inline void mmio_w16(vaddr_t a, u16 v) { *(volatile u16 *)a = v; }
static inline void mmio_w32(vaddr_t a, u32 v) { *(volatile u32 *)a = v; }
static inline void mmio_w64(vaddr_t a, u64 v) { *(volatile u64 *)a = v; }
