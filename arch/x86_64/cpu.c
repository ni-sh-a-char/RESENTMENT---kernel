/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - x86_64 CPU services.
 *
 * Feature detection here is not trivia: the AI subsystem picks kernels by what
 * the CPU can do (AVX2 versus AVX512 versus plain SSE2 changes a matmul by
 * several times), the crypto layer uses AES-NI and SHA extensions when they
 * exist, and the scheduler needs to know whether the TSC is invariant before
 * it trusts a timestamp.
 */
#include <arch/x86.h>
#include <rk/arch.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/sched.h>
#include <rk/panic.h>
#include <rk/time.h>
#include <rk/errno.h>

#undef RK_SUBSYS
#define RK_SUBSYS "cpu"

static char  cpu_vendor[13];
static char  cpu_brand[49];
static u64   cpu_feats;
static u32   cpu_online = 1;
static size_t fpu_size = 512;

/* Per-CPU block, reached through GS. Keeping the running thread here is what
 * lets thread_current() be two instructions instead of a lookup.
 *
 * The field offsets are load-bearing: syscall_entry.asm indexes this structure
 * directly, so the static assertions below are the contract between the two
 * files and must be updated together. */
struct percpu {
	struct percpu *self;         /*  0 */
	u32            id;           /*  8 */
	u32            apic_id;      /* 12 */
	vaddr_t        kstack_top;   /* 16 - syscall entry loads rsp from here */
	u64            scratch;      /* 24 - syscall entry parks the user rsp here */
	struct thread *current;      /* 32 */
	u64            ticks;        /* 40 */
	u64            idle_ns;      /* 48 */
};

RK_STATIC_ASSERT(offsetof(struct percpu, kstack_top) == 16,
                 "syscall_entry.asm reads the kernel stack at gs:16");
RK_STATIC_ASSERT(offsetof(struct percpu, scratch) == 24,
                 "syscall_entry.asm parks the user stack at gs:24");

static struct percpu cpus[RK_MAX_CPUS];

static inline struct percpu *this_cpu(void)
{
	struct percpu *p;
	__asm__ __volatile__("movq %%gs:0, %0" : "=r"(p));
	return p;
}

const char *arch_name(void) { return "x86_64"; }
const char *arch_cpu_model(void) { return cpu_brand[0] ? cpu_brand : cpu_vendor; }
u64  arch_cpu_features(void) { return cpu_feats; }
u32  arch_cpu_count(void) { return cpu_online; }

u32 arch_cpu_id(void)
{
	/* Before the per-CPU block is installed, GS is zero and dereferencing it
	 * would fault. Everything early runs on CPU 0 anyway. */
	u64 gs = rdmsr(MSR_GS_BASE);
	if (!gs)
		return 0;
	return this_cpu()->id;
}

void x86_percpu_init(u32 id, u32 apic_id)
{
	struct percpu *p = &cpus[id];
	p->self    = p;
	p->id      = id;
	p->apic_id = apic_id;
	p->current = NULL;
	wrmsr(MSR_GS_BASE, (u64)(uintptr_t)p);
	wrmsr(MSR_KGS_BASE, 0);
}

struct thread *arch_current_thread(void)
{
	if (!rdmsr(MSR_GS_BASE))
		return NULL;
	return this_cpu()->current;
}

void arch_set_current_thread(struct thread *t)
{
	if (!rdmsr(MSR_GS_BASE))
		return;
	struct percpu *p = this_cpu();
	p->current = t;
	/* Keep the syscall entry stack pointing at whatever is running, so a
	 * SYSCALL from user mode lands on this thread's kernel stack. */
	if (t && t->kstack) {
		p->kstack_top = t->kstack + t->kstack_size;
		x86_tss_set_kernel_stack(p->kstack_top);
	}
}

/* ------------------------------------------------------------- detection */

static void set_brand(void)
{
	u32 r[13];
	u32 max_ext;
	cpuid_raw(0x80000000, 0, &max_ext, &r[1], &r[2], &r[3]);
	if (max_ext < 0x80000004) {
		strlcpy(cpu_brand, cpu_vendor, sizeof(cpu_brand));
		return;
	}
	u32 *w = (u32 *)cpu_brand;
	for (u32 leaf = 0x80000002, i = 0; leaf <= 0x80000004; leaf++, i += 4)
		cpuid_raw(leaf, 0, &w[i], &w[i + 1], &w[i + 2], &w[i + 3]);
	cpu_brand[48] = '\0';
}

void x86_cpu_detect(void)
{
	u32 a, b, c, d;

	cpuid_raw(0, 0, &a, &b, &c, &d);
	*(u32 *)&cpu_vendor[0] = b;
	*(u32 *)&cpu_vendor[4] = d;
	*(u32 *)&cpu_vendor[8] = c;
	cpu_vendor[12] = '\0';
	u32 max_leaf = a;

	cpuid_raw(1, 0, &a, &b, &c, &d);
	if (d & (1u << 0))  cpu_feats |= RK_FEAT_FPU;
	if (d & (1u << 26)) cpu_feats |= RK_FEAT_SIMD;      /* SSE2 */
	if (c & (1u << 25)) cpu_feats |= RK_FEAT_AES;
	if (c & (1u << 30)) cpu_feats |= RK_FEAT_RDRAND;
	if (c & (1u << 21)) cpu_feats |= RK_FEAT_X2APIC;
	if (c & (1u << 17)) cpu_feats |= RK_FEAT_PCID;

	if (max_leaf >= 7) {
		cpuid_raw(7, 0, &a, &b, &c, &d);
		if (b & (1u << 5))  cpu_feats |= RK_FEAT_SIMD256;  /* AVX2 */
		if (b & (1u << 16)) cpu_feats |= RK_FEAT_SIMD512;  /* AVX512F */
		if (b & (1u << 29)) cpu_feats |= RK_FEAT_SHA;
		if (b & (1u << 7))  cpu_feats |= RK_FEAT_SMEP;
		if (b & (1u << 20)) cpu_feats |= RK_FEAT_SMAP;
		cpuid_raw(7, 1, &a, &b, &c, &d);
		if (a & (1u << 5))  cpu_feats |= RK_FEAT_BF16;
	}

	cpuid_raw(0x80000001, 0, &a, &b, &c, &d);
	if (d & (1u << 20)) cpu_feats |= RK_FEAT_NX;
	if (d & (1u << 26)) cpu_feats |= RK_FEAT_1GPAGE;

	cpuid_raw(0x80000007, 0, &a, &b, &c, &d);
	if (d & (1u << 8)) cpu_feats |= RK_FEAT_INVARTSC;

	set_brand();

	/* Turn on the protections the hardware offers. SMEP and SMAP between them
	 * remove an entire family of privilege escalations by making kernel
	 * execution of, and accidental access to, user pages faults. */
	u64 cr4 = read_cr4();
	if (cpu_feats & RK_FEAT_SMEP) cr4 |= (1ull << 20);
	if (cpu_feats & RK_FEAT_SMAP) cr4 |= (1ull << 21);
	cr4 |= (1ull << 9);    /* OSFXSR:  SSE state save */
	cr4 |= (1ull << 10);   /* OSXMMEXCPT */
	write_cr4(cr4);

	/* Bring up the FPU itself: the AI subsystem is the whole point of this
	 * kernel and it cannot run without one. */
	u64 cr0 = read_cr0();
	cr0 &= ~(1ull << 2);   /* clear EM: no x87 emulation */
	cr0 |= (1ull << 1);    /* MP */
	cr0 |= (1ull << 5);    /* NE: native FP exceptions */
	write_cr0(cr0);
	__asm__ __volatile__("fninit");

	if (cpu_feats & RK_FEAT_SIMD256) {
		/* XSAVE and AVX need OSXSAVE plus explicit state enables in XCR0. */
		cpuid_raw(1, 0, &a, &b, &c, &d);
		if (c & (1u << 26)) {
			write_cr4(read_cr4() | (1ull << 18));   /* OSXSAVE */
			u64 xcr0 = 0x7;                          /* x87 | SSE | AVX */
			__asm__ __volatile__("xsetbv" :: "c"(0), "a"((u32)xcr0), "d"(0));
			cpuid_raw(0x0D, 0, &a, &b, &c, &d);
			fpu_size = c ? c : 512;
		}
	}

	pr_info("%s", arch_cpu_model());
	pr_info("features:%s%s%s%s%s%s%s%s%s",
	        (cpu_feats & RK_FEAT_SIMD)    ? " sse2"    : "",
	        (cpu_feats & RK_FEAT_SIMD256) ? " avx2"    : "",
	        (cpu_feats & RK_FEAT_SIMD512) ? " avx512"  : "",
	        (cpu_feats & RK_FEAT_AES)     ? " aes"     : "",
	        (cpu_feats & RK_FEAT_SHA)     ? " sha"     : "",
	        (cpu_feats & RK_FEAT_RDRAND)  ? " rdrand"  : "",
	        (cpu_feats & RK_FEAT_SMEP)    ? " smep"    : "",
	        (cpu_feats & RK_FEAT_SMAP)    ? " smap"    : "",
	        (cpu_feats & RK_FEAT_INVARTSC)? " invtsc"  : "");
}

/* An application processor resets with CR0 and CR4 at their default values, so
 * every protection and every FPU enable the boot processor turned on has to be
 * turned on again here. Detection itself is not repeated: the cores are
 * homogeneous, and a second copy of the feature banner in the log is noise.
 *
 * ponytail: assumes homogeneous cores. Re-run CPUID per core if RESENTMENT is
 * ever brought up on a big.LITTLE-style x86 part with asymmetric ISA. */
void x86_cpu_detect_secondary(void)
{
	u64 cr4 = read_cr4();
	if (cpu_feats & RK_FEAT_SMEP) cr4 |= (1ull << 20);
	if (cpu_feats & RK_FEAT_SMAP) cr4 |= (1ull << 21);
	cr4 |= (1ull << 9) | (1ull << 10);      /* OSFXSR, OSXMMEXCPT */
	write_cr4(cr4);

	u64 cr0 = read_cr0();
	cr0 &= ~(1ull << 2);
	cr0 |= (1ull << 1) | (1ull << 5);
	write_cr0(cr0);
	__asm__ __volatile__("fninit");

	if (cpu_feats & RK_FEAT_SIMD256) {
		u32 a, b, c, d;
		cpuid_raw(1, 0, &a, &b, &c, &d);
		if (c & (1u << 26)) {
			write_cr4(read_cr4() | (1ull << 18));
			__asm__ __volatile__("xsetbv" :: "c"(0), "a"(0x7u), "d"(0));
		}
	}
}

/* ------------------------------------------------------------- execution */

void arch_halt(void)
{
	for (;;)
		__asm__ __volatile__("cli; hlt");
}

void arch_idle(void)  { __asm__ __volatile__("sti; hlt"); }
void arch_cpu_relax(void) { __asm__ __volatile__("pause"); }

void arch_irq_enable(void)  { __asm__ __volatile__("sti"); }
void arch_irq_disable(void) { __asm__ __volatile__("cli"); }

bool arch_irq_enabled(void)
{
	u64 f;
	__asm__ __volatile__("pushfq; popq %0" : "=r"(f));
	return (f & (1ull << 9)) != 0;
}

unsigned long arch_irq_save(void)
{
	u64 f;
	__asm__ __volatile__("pushfq; popq %0; cli" : "=r"(f) :: "memory");
	return (unsigned long)f;
}

void arch_irq_restore(unsigned long flags)
{
	if (flags & (1ull << 9))
		__asm__ __volatile__("sti" ::: "memory");
}

void arch_reboot(void)
{
	/* Try the keyboard controller, then a triple fault. One of them always
	 * works, and the second one always works. */
	for (int i = 0; i < 100; i++) {
		if (!(arch_inb(0x64) & 0x02))
			break;
	}
	arch_outb(0x64, 0xFE);
	rk_mdelay(100);

	struct { u16 limit; u64 base; } __packed null_idt = { 0, 0 };
	__asm__ __volatile__("lidt %0; int3" :: "m"(null_idt));
	arch_halt();
}

void arch_poweroff(void)
{
	/* ACPI is the correct answer; these are the two the emulators accept, and
	 * on real hardware without ACPI there is nothing better to try. */
	arch_outw(0x604, 0x2000);    /* QEMU */
	arch_outw(0xB004, 0x2000);   /* Bochs */
	arch_outw(0x4004, 0x3400);   /* VirtualBox */
	arch_halt();
}

/* ------------------------------------------------------------------ FPU */

size_t arch_fpu_state_size(void) { return fpu_size; }

void arch_fpu_save(struct thread *t)
{
	if (t && t->fpu_state)
		__asm__ __volatile__("fxsave (%0)" :: "r"(t->fpu_state) : "memory");
}

void arch_fpu_restore(struct thread *t)
{
	if (t && t->fpu_state)
		__asm__ __volatile__("fxrstor (%0)" :: "r"(t->fpu_state) : "memory");
}

/* ---------------------------------------------------------------- random */

size_t arch_hw_random(void *buf, size_t len)
{
	if (!(cpu_feats & RK_FEAT_RDRAND))
		return 0;

	u8 *p = buf;
	size_t done = 0;
	while (done < len) {
		u64 v;
		u8  ok = 0;
		/* RDRAND can legitimately fail when the entropy pool is drained; the
		 * SDM says retry ten times and then treat it as unavailable. */
		for (int i = 0; i < 10 && !ok; i++)
			__asm__ __volatile__("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
		if (!ok)
			break;
		size_t n = len - done < 8 ? len - done : 8;
		for (size_t i = 0; i < n; i++)
			p[done + i] = (u8)(v >> (i * 8));
		done += n;
	}
	return done;
}

void x86_set_cpu_online(u32 n) { cpu_online = n; }
