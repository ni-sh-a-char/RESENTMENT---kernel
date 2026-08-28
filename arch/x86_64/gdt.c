/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - GDT and TSS.
 *
 * Long mode barely uses segmentation, but three things still need it: the code
 * segment carries the long-mode and privilege bits, SYSCALL/SYSRET require the
 * selectors to be laid out in a specific order, and the TSS supplies the stack
 * the CPU switches to on a ring transition or a double fault.
 *
 * The IST entries are the reason a stack overflow prints a backtrace here
 * instead of triple faulting: a fault that occurs because the kernel stack is
 * unusable cannot be handled on the kernel stack.
 */
#include <arch/x86.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/mm.h>
#include <rk/panic.h>

#undef RK_SUBSYS
#define RK_SUBSYS "gdt"

struct tss64 {
	u32 reserved0;
	u64 rsp[3];
	u64 reserved1;
	u64 ist[7];
	u64 reserved2;
	u16 reserved3;
	u16 iomap_base;
} __packed;

struct gdt_ptr {
	u16 limit;
	u64 base;
} __packed;

#define GDT_ENTRIES 7   /* null, kcode, kdata, udata, ucode, tss (2 slots) */

#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MCE          3
#define IST_STACK_SIZE   8192

/* Every core needs its own of all of this. The GDT could in principle be
 * shared, but its TSS descriptor cannot: a TSS holds the ring-0 stack pointer,
 * so two cores sharing one would take interrupts on each other's stacks. The
 * hardware says so too - the descriptor's busy bit means a second LTR against
 * the same descriptor faults. The fault stacks are per core for the same
 * reason: a double fault on two cores at once must not land on one stack.
 *
 * CPU 0 is static because it is set up long before there is a heap; the rest
 * come from the heap, so a 256-core cap does not cost megabytes of .bss on a
 * machine with two cores. */
struct cpu_desc {
	u64          gdt[GDT_ENTRIES];
	struct tss64 tss;
	u8           ist_df[IST_STACK_SIZE];
	u8           ist_nmi[IST_STACK_SIZE];
	u8           ist_mce[IST_STACK_SIZE];
} __aligned(16);

static struct cpu_desc  bsp_desc __aligned(16);
static struct cpu_desc *desc[RK_MAX_CPUS];

static u64 gdt_entry(u32 base, u32 limit, u8 access, u8 flags)
{
	u64 d = 0;
	d |= (u64)(limit & 0xFFFF);
	d |= (u64)(base & 0xFFFFFF) << 16;
	d |= (u64)access << 40;
	d |= (u64)((limit >> 16) & 0xF) << 48;
	d |= (u64)(flags & 0xF) << 52;
	d |= (u64)((base >> 24) & 0xFF) << 56;
	return d;
}

void x86_tss_set_kernel_stack(vaddr_t sp)
{
	struct cpu_desc *d = desc[arch_cpu_id() % RK_MAX_CPUS];
	if (d)
		d->tss.rsp[0] = sp;
}

void x86_gdt_init(u32 cpu)
{
	RK_ASSERT(cpu < RK_MAX_CPUS);

	struct cpu_desc *d = cpu == 0 ? &bsp_desc
	                              : kmalloc_aligned(sizeof(*d), 16);
	if (!d)
		panic("cpu%u: no memory for a descriptor table", cpu);
	desc[cpu] = d;

	u64 *gdt = d->gdt;
	struct tss64 *tssp = &d->tss;

	memset(tssp, 0, sizeof(*tssp));
	tssp->ist[IST_DOUBLE_FAULT - 1] = (u64)(uintptr_t)d->ist_df  + IST_STACK_SIZE;
	tssp->ist[IST_NMI - 1]          = (u64)(uintptr_t)d->ist_nmi + IST_STACK_SIZE;
	tssp->ist[IST_MCE - 1]          = (u64)(uintptr_t)d->ist_mce + IST_STACK_SIZE;
	/* An I/O map base past the TSS limit means no port access from ring 3,
	 * which is what we want until a driver task asks for a port capability. */
	tssp->iomap_base = sizeof(*tssp);

	gdt[0] = 0;
	/* access: P=1 DPL S=1 E DC RW A   flags: G DB L AVL */
	gdt[1] = gdt_entry(0, 0, 0x9A, 0xA);   /* 0x08 ring 0 code, L=1 */
	gdt[2] = gdt_entry(0, 0, 0x92, 0xC);   /* 0x10 ring 0 data */
	gdt[3] = gdt_entry(0, 0, 0xF2, 0xC);   /* 0x18 ring 3 data */
	gdt[4] = gdt_entry(0, 0, 0xFA, 0xA);   /* 0x20 ring 3 code, L=1 */

	/* The TSS descriptor is 16 bytes and occupies two GDT slots. */
	u64 base  = (u64)(uintptr_t)tssp;
	u32 limit = sizeof(*tssp) - 1;
	gdt[5] = gdt_entry((u32)base, limit, 0x89, 0x0);
	gdt[6] = (base >> 32) & 0xFFFFFFFFull;

	struct gdt_ptr ptr = { sizeof(d->gdt) - 1, (u64)(uintptr_t)gdt };
	__asm__ __volatile__("lgdt %0" :: "m"(ptr) : "memory");

	/* Reload CS through a far return, which is the only way to change it in
	 * long mode, then the data selectors. */
	__asm__ __volatile__(
		"pushq %[kcode]\n\t"
		"leaq  1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n\t"
		"1:\n\t"
		"mov %[kdata], %%ax\n\t"
		"mov %%ax, %%ds\n\t"
		"mov %%ax, %%es\n\t"
		"mov %%ax, %%ss\n\t"
		"xor %%ax, %%ax\n\t"
		"mov %%ax, %%fs\n\t"
		"mov %%ax, %%gs\n\t"
		:
		: [kcode] "i"((u64)SEL_KCODE), [kdata] "i"((u32)SEL_KDATA)
		: "rax", "memory");

	__asm__ __volatile__("ltr %w0" :: "r"((u16)SEL_TSS));
}
