/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - aarch64 support.
 *
 * One file, because the aarch64 HAL is genuinely smaller than the x86 one:
 * there is no segmentation, no PIC, no legacy timer, and the interrupt
 * controller and UART are described by the device tree rather than probed at
 * fixed ports.
 *
 * Covers the QEMU virt machine and any board close enough to it, which is most
 * of them: a PL011 UART, a GICv2 or GICv3, and the architected generic timer.
 */
#include <rk/arch.h>
#include <rk/boot.h>
#include <rk/console.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/irq.h>
#include <rk/time.h>
#include <rk/mm.h>
#include <rk/sched.h>
#include <rk/syscall.h>
#include <rk/panic.h>

#undef RK_SUBSYS
#define RK_SUBSYS "aarch64"

/* QEMU virt defaults. A device tree overrides all of them; these are what the
 * kernel uses before the tree has been parsed, so that early output works. */
#define PL011_BASE   0x09000000ul
#define GICD_BASE    0x08000000ul
#define GICC_BASE    0x08010000ul
#define RAM_BASE     0x40000000ull

#define DIRECT_MAP_BASE 0xFFFF800000000000ull

/* ------------------------------------------------------------ registers */

#define SYSREG_READ(name)                                    \
	({ u64 __v; __asm__ __volatile__("mrs %0, " #name : "=r"(__v)); __v; })
#define SYSREG_WRITE(name, v)                                \
	__asm__ __volatile__("msr " #name ", %0" :: "r"((u64)(v)))

static char cpu_model[64] = "aarch64";
static u64  cpu_feats;
static u32  cpus_online = 1;
static u64  timer_freq;
static struct thread *current_thread_slot[RK_MAX_CPUS];

const char *arch_name(void) { return "aarch64"; }
const char *arch_cpu_model(void) { return cpu_model; }

/* The logical core number, not MPIDR. MPIDR is an affinity path, not an index:
 * on a two-cluster part the second cluster starts again at zero in Aff0, so
 * using it as an array subscript aliases two cores onto one slot. Each core
 * stashes the number the kernel gave it in TPIDR_EL1 instead. */
u32  arch_cpu_id(void) { return (u32)SYSREG_READ(tpidr_el1); }
u32  arch_cpu_count(void) { return cpus_online; }
u64  arch_cpu_features(void) { return cpu_feats; }

/* -------------------------------------------------------------- PL011 */

static volatile u32 *uart = (volatile u32 *)PL011_BASE;

#define UART_DR   0x00
#define UART_FR   0x18
#define UART_IBRD 0x24
#define UART_FBRD 0x28
#define UART_LCRH 0x2C
#define UART_CR   0x30
#define UART_IMSC 0x38
#define UART_ICR  0x44

static inline u32 uart_read(u32 off) { return *(volatile u32 *)((u8 *)uart + off); }
static inline void uart_write(u32 off, u32 v) { *(volatile u32 *)((u8 *)uart + off) = v; }

void rk_serial_putc(char c)
{
	/* Bounded: a UART with no receiver must not wedge the boot. */
	for (int i = 0; i < 100000; i++)
		if (!(uart_read(UART_FR) & (1 << 5)))
			break;
	uart_write(UART_DR, (u32)(u8)c);
}

static void pl011_write(struct rk_console *c, const char *s, size_t n)
{
	(void)c;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\n')
			rk_serial_putc('\r');
		rk_serial_putc(s[i]);
	}
}

static void pl011_putc(struct rk_console *c, char ch) { pl011_write(c, &ch, 1); }

static const u8 ansi[16] = { 30, 34, 32, 36, 31, 35, 33, 37, 90, 94, 92, 96, 91, 95, 93, 97 };

static void pl011_color(struct rk_console *c, u8 fg, u8 bg)
{
	(void)c; (void)bg;
	char buf[12];
	int n = snprintf(buf, sizeof(buf), "\033[%um", ansi[fg & 0xF]);
	pl011_write(NULL, buf, (size_t)n);
}

static void pl011_clear(struct rk_console *c)
{
	(void)c;
	pl011_write(NULL, "\033[2J\033[H", 7);
}

/* Polled receive; see the x86 port for why this exists alongside the
 * interrupt path. FR bit 4 is the receive-FIFO-empty flag. */
static int pl011_getc(struct rk_console *c)
{
	(void)c;
	if (uart_read(UART_FR) & (1 << 4))
		return -1;
	return (int)(uart_read(UART_DR) & 0xFF);
}

static struct rk_console pl011_console = {
	.name = "ttyAMA0",
	.putc = pl011_putc,
	.write = pl011_write,
	.clear = pl011_clear,
	.set_color = pl011_color,
	.getc = pl011_getc,
	.width = 80,
	.height = 25,
};

void rk_serial_early_init(void)
{
	uart_write(UART_CR, 0);            /* disable while configuring */
	uart_write(UART_ICR, 0x7FF);       /* clear every pending interrupt */
	uart_write(UART_IBRD, 26);         /* 115200 from a 48 MHz clock */
	uart_write(UART_FBRD, 3);
	uart_write(UART_LCRH, (3 << 5) | (1 << 4));   /* 8N1, FIFO on */
	uart_write(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));   /* enable, tx, rx */
}

void rk_serial_console_init(void) { rk_console_register(&pl011_console); }

/* --------------------------------------------------------------- GICv2 */

#define GICD_CTLR      0x000
#define GICD_ISENABLER 0x100
#define GICD_ICENABLER 0x180
#define GICD_IPRIORITY 0x400
#define GICD_ITARGETSR 0x800
#define GICD_ICFGR     0xC00

#define GICC_CTLR      0x00
#define GICC_PMR       0x04
#define GICC_IAR       0x0C
#define GICC_EOIR      0x10

static volatile u8 *gicd = (volatile u8 *)GICD_BASE;
static volatile u8 *gicc = (volatile u8 *)GICC_BASE;

static void gic_init_cpu(void);

static void gic_init(void)
{
	*(volatile u32 *)(gicd + GICD_CTLR) = 0;

	/* Mask everything, route to CPU 0, default priority. Unmasking is the
	 * driver's decision, exactly as on x86. */
	for (u32 i = 0; i < 32; i++)
		*(volatile u32 *)(gicd + GICD_ICENABLER + i * 4) = 0xFFFFFFFF;
	for (u32 i = 8; i < 256; i++)
		*(volatile u8 *)(gicd + GICD_IPRIORITY + i) = 0xA0;
	for (u32 i = 8; i < 256; i++)
		*(volatile u8 *)(gicd + GICD_ITARGETSR + i) = 0x01;

	*(volatile u32 *)(gicd + GICD_CTLR) = 1;
	gic_init_cpu();
}

/* The CPU interface is per core and is not covered by the distributor set-up
 * above: a secondary that skips this takes no interrupts at all, which looks
 * exactly like a core that never started. */
static void gic_init_cpu(void)
{
	*(volatile u32 *)(gicc + GICC_PMR) = 0xF0;   /* accept below this priority */
	*(volatile u32 *)(gicc + GICC_CTLR) = 1;
}

void rk_irq_mask_arch(u32 irq, bool masked)
{
	u32 reg = irq / 32, bit = irq % 32;
	if (masked)
		*(volatile u32 *)(gicd + GICD_ICENABLER + reg * 4) = 1u << bit;
	else
		*(volatile u32 *)(gicd + GICD_ISENABLER + reg * 4) = 1u << bit;
}

void rk_irq_eoi_arch(u32 vector)
{
	*(volatile u32 *)(gicc + GICC_EOIR) = vector;
}

void aarch64_irq(void);

void aarch64_irq(void)
{
	u32 iar = *(volatile u32 *)(gicc + GICC_IAR);
	u32 id = iar & 0x3FF;

	/* 1023 is the spurious id the GIC returns when nothing is pending. */
	if (id == 1023)
		return;

	/* Ids below 16 are software generated: they came from another core, not
	 * from a device, so they are acknowledged here rather than dispatched.
	 * A software generated interrupt is acknowledged with the whole IAR
	 * value, source core field included. */
	if (id < 16) {
		*(volatile u32 *)(gicc + GICC_EOIR) = iar;
		rk_ipi_handle(RK_IPI_RESCHED + id);
		return;
	}

	rk_irq_dispatch(id);
}

/* ------------------------------------------------------------ exceptions */

/* Index of the saved ELR within the exception frame that boot.S builds, in
 * 64-bit words. The two files have to agree; changing one without the other is
 * an evening well spent elsewhere. */
#define FRAME_ELR_WORD 22

void aarch64_sync_exception(u64 esr, u64 far, u64 elr, u64 *frame);
void aarch64_serror(u64 esr);

/* Set while probing for the end of RAM. An access past the last populated
 * byte takes an external abort on most machines, so the probe arranges to
 * survive one instead of guessing how much memory exists. */
static volatile bool probing;
static volatile bool probe_faulted;

void aarch64_sync_exception(u64 esr, u64 far, u64 elr, u64 *frame)
{
	u32 ec = (u32)(esr >> 26);

	if (probing && (ec == 0x25 || ec == 0x24)) {
		probe_faulted = true;
		/* Resume after the faulting instruction. Every A64 instruction is
		 * four bytes, so this is exact rather than a guess. It edits the
		 * saved ELR rather than the register, because the epilogue restores
		 * the register from the frame. */
		frame[FRAME_ELR_WORD] = elr + 4;
		return;
	}

	/* Data and instruction aborts are page faults; everything else is a bug
	 * or a system call. */
	if (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) {
		u32 access = VM_FAULT_READ;
		if (esr & (1 << 6))
			access = VM_FAULT_WRITE;
		if (ec == 0x20 || ec == 0x21)
			access = VM_FAULT_EXEC;
		if (ec == 0x20 || ec == 0x24)
			access |= VM_FAULT_USER;

		struct task *t = task_current();
		struct address_space *as = t ? t->as : as_kernel();
		if (as && vm_handle_fault(as, (vaddr_t)far, access) == RK_OK)
			return;
	}

	if (ec == 0x15) {   /* SVC from AArch64: a system call */
		/* The frame is what SAVE_REGS wrote: x0..x17 in order from the top,
		 * so the arguments and the call number are read straight out of it
		 * and the result is written back over the saved x0. ELR already
		 * points past the SVC, because that is what the hardware sets it to.
		 */
		struct rk_syscall_args a = {
			.nr = frame[8],
			.a0 = frame[0], .a1 = frame[1], .a2 = frame[2],
			.a3 = frame[3], .a4 = frame[4], .a5 = frame[5],
		};
		frame[0] = (u64)rk_syscall_dispatch(&a);
		return;
	}

	panic("unhandled exception: esr %#llx (class %#x) far %#llx elr %#llx "
	      "spsr %#llx",
	      (unsigned long long)esr, ec, (unsigned long long)far,
	      (unsigned long long)elr, (unsigned long long)frame[23]);
}

void aarch64_serror(u64 esr)
{
	panic("system error: esr %#llx", (unsigned long long)esr);
}

/* ---------------------------------------------------------------- timer */

#define CNTV_CTL_ENABLE  1
#define CNTV_CTL_IMASK   2
#define TIMER_IRQ 27      /* virtual timer PPI on the virt machine */

static u64 timer_interval;

static enum rk_irq_result timer_handler(u32 irq, void *dev)
{
	(void)irq; (void)dev;
	/* Re-arm before doing work, so a slow tick does not accumulate drift. */
	SYSREG_WRITE(cntv_tval_el0, timer_interval);
	rk_timer_tick();
	return RK_IRQ_HANDLED;
}

void arch_timer_init(u32 hz)
{
	timer_freq = SYSREG_READ(cntfrq_el0);
	if (!timer_freq)
		timer_freq = 62500000;   /* the virt machine default */

	timer_interval = timer_freq / (hz ? hz : 100);
	SYSREG_WRITE(cntv_tval_el0, timer_interval);
	SYSREG_WRITE(cntv_ctl_el0, CNTV_CTL_ENABLE);

	rk_irq_request(TIMER_IRQ, timer_handler, NULL, NULL, "generic-timer");
	rk_irq_set_timer_line(TIMER_IRQ);
	rk_irq_unmask(TIMER_IRQ);
	pr_info("generic timer at %llu Hz, ticking at %u Hz",
	        (unsigned long long)timer_freq, hz);
}

/* The comparator and the enable bit are per core, and so are the first 32
 * interrupt ids on a GICv2 - the timer's PPI among them. Registering the
 * handler is global and must not be repeated. */
static void arch_timer_init_cpu(void)
{
	SYSREG_WRITE(cntv_tval_el0, timer_interval);
	SYSREG_WRITE(cntv_ctl_el0, CNTV_CTL_ENABLE);
	rk_irq_unmask(TIMER_IRQ);
}

void arch_timer_oneshot(u64 ns)
{
	u64 ticks = (u64)(((__uint128_t)ns * timer_freq) / 1000000000ull);
	SYSREG_WRITE(cntv_tval_el0, ticks);
}

u64 arch_cycles(void) { return SYSREG_READ(cntvct_el0); }
u64 arch_cycles_per_sec(void) { return timer_freq ? timer_freq : 62500000; }

u64 arch_time_ns(void)
{
	u64 f = arch_cycles_per_sec();
	return (u64)(((__uint128_t)arch_cycles() * 1000000000ull) / f);
}

/* ------------------------------------------------------------------ RTC */

/* The PL031, which is the RTC on QEMU's virt machine and on most ARM boards
 * that have one at all. A single register: seconds since the Unix epoch.
 *
 * The base address is the virt machine's. Probing for it properly means
 * walking the device tree for "arm,pl031", and this port cannot rely on having
 * a tree - QEMU hands none to a bare ELF. So the address is assumed and the
 * *answer* is checked instead: a time outside a plausible window is treated as
 * no clock rather than as a clock that is wrong, because Kaalka would
 * otherwise derive epoch keys from nonsense.
 *
 * Without a wall clock, seals have no real time base and every clock angle
 * Kaalka reads is zero - which is exactly the kind of failure that looks like
 * everything working.
 *
 * ponytail: hardcoded base with a sanity check. Read it from the device tree
 * once this port has a tree it can count on. */
#define PL031_BASE  0x09010000ul
#define PL031_DR    0x00            /* data register: seconds since the epoch */

/* 2020-01-01 and 2100-01-01. Wide enough not to reject a badly set clock,
 * narrow enough to reject a register that is not an RTC. */
#define WALLCLOCK_MIN 1577836800ll
#define WALLCLOCK_MAX 4102444800ll

s64 arch_wallclock_unix(void)
{
	volatile const u32 *rtc = (volatile const u32 *)PL031_BASE;
	s64 secs = (s64)rtc[PL031_DR / 4];

	return (secs >= WALLCLOCK_MIN && secs < WALLCLOCK_MAX) ? secs : 0;
}

/* --------------------------------------------------------------- memory */

/* The MMU is not optional on this architecture, and not for the reason people
 * expect. With SCTLR_EL1.M clear, every data access is treated as
 * Device-nGnRnE, which forbids unaligned access outright - so an ordinary
 * structure copy that the compiler turned into an unaligned load takes an
 * alignment abort. Running "with paging off" is therefore not a simpler mode,
 * it is a broken one, and the port has to turn the MMU on before it runs any
 * real C code.
 *
 * The layout is a 39-bit address space with 4 KiB granules: three levels, and
 * the first lookup is at level 1 where each entry covers 1 GiB. That is enough
 * to describe a board with one block per gigabyte, and finer mappings split
 * down to level 3 on demand.
 */

#define PTE_VALID   (1ull << 0)
#define PTE_TABLE   (1ull << 1)   /* at L1/L2: table rather than block */
#define PTE_PAGE    (1ull << 1)   /* at L3: page (same bit, opposite sense) */
#define PTE_ATTR(i) ((u64)(i) << 2)
#define PTE_NS      (1ull << 5)
#define PTE_AP_RW   (0ull << 6)
#define PTE_AP_RO   (2ull << 6)
#define PTE_AP_USER (1ull << 6)
#define PTE_SH_INNER (3ull << 8)
#define PTE_AF      (1ull << 10)  /* access flag; a miss faults without it */
#define PTE_NG      (1ull << 11)
#define PTE_PXN     (1ull << 53)
#define PTE_UXN     (1ull << 54)

#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ull

/* MAIR slots: 0 is normal write-back cacheable, 1 is device nGnRnE. */
#define MAIR_NORMAL 0
#define MAIR_DEVICE 1
#define MAIR_VALUE  ((0xFFull << (8 * MAIR_NORMAL)) | (0x00ull << (8 * MAIR_DEVICE)))

#define L1_SHIFT 30
#define L2_SHIFT 21
#define L3_SHIFT 12
#define IDX(va, shift) (((va) >> (shift)) & 0x1FF)

static u64 *kernel_l1;

vaddr_t arch_phys_to_virt(paddr_t pa) { return (vaddr_t)pa; }
paddr_t arch_virt_to_phys(vaddr_t va) { return (paddr_t)va; }

static u64 prot_to_pte(u32 prot, bool device)
{
	u64 e = PTE_VALID | PTE_AF | PTE_SH_INNER;

	e |= device ? PTE_ATTR(MAIR_DEVICE) : PTE_ATTR(MAIR_NORMAL);
	e |= (prot & RK_PROT_WRITE) ? PTE_AP_RW : PTE_AP_RO;

	/* The two execute-never bits are not symmetric and must not be set
	 * together by accident.
	 *
	 * UXN forbids execution at EL0, PXN forbids it at EL1. A kernel page
	 * wants UXN always - EL0 has no business executing it - and PXN only when
	 * it is not code. A user page wants the mirror image: PXN always, because
	 * the kernel executing user memory is precisely what SMEP prevents on
	 * x86 and the same argument applies here, and UXN only when the segment
	 * is not executable.
	 *
	 * Setting UXN unconditionally, as this did, makes every user page
	 * non-executable - so a program loads, is entered, and takes an
	 * instruction abort on its first instruction, forever. */
	if (prot & RK_PROT_USER) {
		e |= PTE_AP_USER | PTE_NG | PTE_PXN;
		if (!(prot & RK_PROT_EXEC))
			e |= PTE_UXN;
	} else {
		e |= PTE_UXN;
		if (!(prot & RK_PROT_EXEC))
			e |= PTE_PXN;
	}
	return e;
}

static u64 *table_at(u64 entry)
{
	return (u64 *)(uintptr_t)(entry & PTE_ADDR_MASK);
}

/* Break a block entry into a table of smaller ones covering the same memory
 * with the same attributes. Same reasoning as the x86 port: without this, a
 * permission change on one page silently changes a gigabyte. */
static bool split_block(u64 *table, size_t idx, unsigned level)
{
	u64 e = table[idx];
	if (!(e & PTE_VALID) || (e & PTE_TABLE))
		return true;

	paddr_t pa = pmm_alloc_page();
	if (!pa)
		return false;

	u64 *child = (u64 *)(uintptr_t)pa;
	u64  attrs = e & ~PTE_ADDR_MASK;
	u64  base  = e & PTE_ADDR_MASK;
	u64  step  = (level == 1) ? (1ull << L2_SHIFT) : (1ull << L3_SHIFT);

	for (size_t i = 0; i < 512; i++)
		child[i] = (base + i * step) | attrs | (level == 2 ? PTE_PAGE : 0);

	table[idx] = pa | PTE_VALID | PTE_TABLE;
	__asm__ __volatile__("dsb ishst" ::: "memory");
	return true;
}

static u64 *walk(u64 *table, size_t idx, unsigned level, bool create)
{
	u64 e = table[idx];

	if (!(e & PTE_VALID)) {
		if (!create)
			return NULL;
		paddr_t pa = pmm_alloc_page();
		if (!pa)
			return NULL;
		memset((void *)(uintptr_t)pa, 0, RK_PAGE_SIZE);
		table[idx] = pa | PTE_VALID | PTE_TABLE;
		__asm__ __volatile__("dsb ishst" ::: "memory");
		return (u64 *)(uintptr_t)pa;
	}
	if (!(e & PTE_TABLE)) {
		if (!create || !split_block(table, idx, level))
			return NULL;
		e = table[idx];
	}
	return table_at(e);
}

pgtable_t arch_pgtable_kernel(void) { return (pgtable_t)kernel_l1; }

pgtable_t arch_pgtable_create(void)
{
	paddr_t pa = pmm_alloc_page();
	if (!pa)
		return NULL;

	u64 *l1 = (u64 *)(uintptr_t)pa;
	memset(l1, 0, RK_PAGE_SIZE);

	/* Every address space carries the kernel's mappings.
	 *
	 * This port translates through TTBR0 only, so a table without them would
	 * unmap the kernel the instant it was installed - the very next
	 * instruction fetch would fault, inside a fault handler that is also no
	 * longer mapped. On x86_64 the equivalent is copying the top half of the
	 * PML4; here it is the bottom eight gigabytes, which is why user programs
	 * are placed above that rather than at the traditional low address.
	 *
	 * The entries are block descriptors covering RAM and the device region,
	 * so nothing below is shared by pointer and a user table can be freed
	 * without touching kernel structures. */
	if (kernel_l1)
		for (size_t i = 0; i < 8; i++)
			l1[i] = kernel_l1[i];

	__asm__ __volatile__("dsb ishst" ::: "memory");
	return (pgtable_t)(uintptr_t)pa;
}

void arch_pgtable_destroy(pgtable_t p)
{
	if (p && (u64 *)p != kernel_l1)
		pmm_free_page((paddr_t)(uintptr_t)p);
}

void arch_pgtable_switch(pgtable_t p)
{
	if (!p)
		return;
	SYSREG_WRITE(ttbr0_el1, (u64)(uintptr_t)p);
	__asm__ __volatile__("isb; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}

static int map_one(u64 *l1, vaddr_t va, paddr_t pa, u64 flags)
{
	u64 *l2 = walk(l1, IDX(va, L1_SHIFT), 1, true);
	if (!l2)
		return RK_ENOMEM;
	u64 *l3 = walk(l2, IDX(va, L2_SHIFT), 2, true);
	if (!l3)
		return RK_ENOMEM;

	l3[IDX(va, L3_SHIFT)] = (pa & PTE_ADDR_MASK) | flags | PTE_PAGE;
	return RK_OK;
}

int arch_map(pgtable_t pt, vaddr_t va, paddr_t pa, size_t len, u32 prot)
{
	u64 *l1 = pt ? (u64 *)pt : kernel_l1;
	if (!l1)
		return (va == (vaddr_t)pa) ? RK_OK : RK_ENOTSUP;
	if (!IS_ALIGNED(va, RK_PAGE_SIZE) || !IS_ALIGNED(pa, RK_PAGE_SIZE))
		return RK_EINVAL;

	/* Anything at or above where RAM starts is normal memory; below it, on
	 * every board this targets, is the device region. */
	bool device = pa < RAM_BASE;
	u64 flags = prot_to_pte(prot, device);

	for (size_t off = 0; off < len; off += RK_PAGE_SIZE) {
		int r = map_one(l1, va + off, pa + off, flags);
		if (r != RK_OK)
			return r;
		/* Installing a translation is not enough: the old one has to be
		 * thrown away. ARM permits an implementation to hold a cached entry
		 * for an address even when the table said it was invalid, and it
		 * certainly holds one when this address was mapped before. Without
		 * this, a freshly mapped page keeps resolving through whatever the
		 * TLB remembers - writes land somewhere else and reads come back
		 * zero, intermittently, depending on what evicted the entry. Unmap
		 * and protect already did this; map did not. */
		arch_tlb_flush_page(va + off);
	}
	__asm__ __volatile__("dsb ishst; isb" ::: "memory");
	return RK_OK;
}

static u64 *find_leaf(u64 *l1, vaddr_t va, size_t *page_size, bool split)
{
	if (!l1)
		return NULL;

	size_t i1 = IDX(va, L1_SHIFT);
	if ((l1[i1] & PTE_VALID) && !(l1[i1] & PTE_TABLE) && !split) {
		*page_size = 1ull << L1_SHIFT;
		return &l1[i1];
	}
	u64 *l2 = walk(l1, i1, 1, split);
	if (!l2)
		return NULL;

	size_t i2 = IDX(va, L2_SHIFT);
	if ((l2[i2] & PTE_VALID) && !(l2[i2] & PTE_TABLE) && !split) {
		*page_size = 1ull << L2_SHIFT;
		return &l2[i2];
	}
	u64 *l3 = walk(l2, i2, 2, split);
	if (!l3)
		return NULL;

	*page_size = RK_PAGE_SIZE;
	return &l3[IDX(va, L3_SHIFT)];
}

int arch_unmap(pgtable_t pt, vaddr_t va, size_t len)
{
	u64 *l1 = pt ? (u64 *)pt : kernel_l1;
	size_t done = 0;

	while (done < len) {
		size_t psize = RK_PAGE_SIZE;
		u64 *leaf = find_leaf(l1, va + done, &psize, true);
		if (leaf) {
			*leaf = 0;
			arch_tlb_flush_page(va + done);
		}
		done += psize;
	}
	return RK_OK;
}

int arch_protect(pgtable_t pt, vaddr_t va, size_t len, u32 prot)
{
	u64 *l1 = pt ? (u64 *)pt : kernel_l1;
	size_t done = 0;

	while (done < len) {
		vaddr_t cur = va + done;
		size_t psize = RK_PAGE_SIZE;
		u64 *leaf = find_leaf(l1, cur, &psize, false);
		if (!leaf) {
			done += RK_PAGE_SIZE;
			continue;
		}
		if (!(*leaf & PTE_TABLE) &&
		    (!IS_ALIGNED(cur, psize) || len - done < psize)) {
			leaf = find_leaf(l1, cur, &psize, true);
			if (!leaf) {
				done += RK_PAGE_SIZE;
				continue;
			}
		}
		if (*leaf & PTE_VALID) {
			bool device = (*leaf & PTE_ATTR(3)) == PTE_ATTR(MAIR_DEVICE);
			*leaf = (*leaf & PTE_ADDR_MASK) | prot_to_pte(prot, device) |
			        (psize == RK_PAGE_SIZE ? PTE_PAGE : 0);
			arch_tlb_flush_page(cur);
		}
		done += psize;
	}
	__asm__ __volatile__("dsb ish; isb" ::: "memory");
	return RK_OK;
}

bool arch_translate(pgtable_t pt, vaddr_t va, paddr_t *pa, u32 *prot)
{
	u64 *l1 = pt ? (u64 *)pt : kernel_l1;
	size_t psize = RK_PAGE_SIZE;
	u64 *leaf = find_leaf(l1, va, &psize, false);

	if (!leaf || !(*leaf & PTE_VALID))
		return false;
	if (pa)
		*pa = (*leaf & PTE_ADDR_MASK) + (va & (psize - 1));
	if (prot) {
		*prot = RK_PROT_READ;
		if ((*leaf & PTE_AP_RO) != PTE_AP_RO)
			*prot |= RK_PROT_WRITE;
		if (!(*leaf & PTE_PXN))
			*prot |= RK_PROT_EXEC;
		if (*leaf & PTE_AP_USER)
			*prot |= RK_PROT_USER;
	}
	return true;
}

/* The initial map: one level-1 table with gigabyte blocks. Device memory below
 * the RAM base, normal memory above it, identity mapped. Built before the page
 * allocator exists, so the table is a static array rather than an allocation.
 */
static u64 boot_l1[512] __aligned(4096);

static void mmu_enable_cpu(void);

static void mmu_enable(void)
{
	u64 ram_base = RAM_BASE;

	/* volatile, and this is load-bearing rather than superstition.
	 *
	 * This loop runs with the MMU still off, which means every access is
	 * Device-nGnRnE. The compiler would happily merge two adjacent stores
	 * into one `stp`, and a 16-byte pair store to an 8-byte-aligned address
	 * is an alignment fault on device memory. volatile forces one store per
	 * entry, which is exactly what is legal here. */
	volatile u64 *t = boot_l1;

	for (size_t i = 0; i < 8; i++) {
		u64 pa = (u64)i << L1_SHIFT;
		bool device = pa < ram_base;
		u64 e = pa | PTE_VALID | PTE_AF | PTE_SH_INNER |
		        (device ? PTE_ATTR(MAIR_DEVICE) : PTE_ATTR(MAIR_NORMAL)) |
		        PTE_AP_RW | PTE_UXN;
		if (device)
			e |= PTE_PXN;            /* never execute from MMIO */
		t[i] = e;
	}
	/* Eight gigabytes described is enough for every board this targets, and
	 * .bss was zeroed by the boot stub, so the rest is already invalid. */
	kernel_l1 = boot_l1;

	mmu_enable_cpu();
}

/* Everything above builds tables; this turns the MMU on against tables that
 * already exist. A secondary core runs only this half. */
static void mmu_enable_cpu(void)
{
	SYSREG_WRITE(mair_el1, MAIR_VALUE);

	/* T0SZ 25 gives a 39-bit address space; inner and outer write-back
	 * write-allocate; inner shareable; 4 KiB granule. TTBR1 walks are
	 * disabled because this port is identity mapped and has no higher half. */
	u64 tcr = (25ull << 0)          /* T0SZ */
	        | (1ull << 8)           /* IRGN0 write-back */
	        | (1ull << 10)          /* ORGN0 write-back */
	        | (3ull << 12)          /* SH0 inner shareable */
	        | (0ull << 14)          /* TG0 4 KiB */
	        | (1ull << 23)          /* EPD1: no TTBR1 walks */
	        | (2ull << 32);         /* IPS 40-bit physical */
	SYSREG_WRITE(tcr_el1, tcr);
	SYSREG_WRITE(ttbr0_el1, (u64)(uintptr_t)kernel_l1);

	__asm__ __volatile__("dsb ish; isb" ::: "memory");
	__asm__ __volatile__("tlbi vmalle1; dsb ish; isb" ::: "memory");

	u64 sctlr = SYSREG_READ(sctlr_el1);
	sctlr |= (1ull << 0);    /* M: MMU on */
	sctlr |= (1ull << 2);    /* C: data cache */
	sctlr |= (1ull << 12);   /* I: instruction cache */
	sctlr &= ~(1ull << 1);   /* A: no strict alignment checking */
	SYSREG_WRITE(sctlr_el1, sctlr);
	__asm__ __volatile__("isb" ::: "memory");
}

void arch_tlb_flush_page(vaddr_t va)
{
	__asm__ __volatile__("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
	                     :: "r"(va >> 12) : "memory");
}

void arch_tlb_flush_all(void)
{
	__asm__ __volatile__("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}

/* ------------------------------------------------------------ execution */

void arch_halt(void)
{
	arch_irq_disable();
	for (;;)
		__asm__ __volatile__("wfi");
}

void arch_idle(void) { __asm__ __volatile__("msr daifclr, #2; wfi"); }
void arch_cpu_relax(void) { __asm__ __volatile__("yield"); }

void arch_irq_enable(void)  { __asm__ __volatile__("msr daifclr, #2" ::: "memory"); }
void arch_irq_disable(void) { __asm__ __volatile__("msr daifset, #2" ::: "memory"); }
bool arch_irq_enabled(void) { return !(SYSREG_READ(daif) & (1 << 7)); }

unsigned long arch_irq_save(void)
{
	unsigned long f = (unsigned long)SYSREG_READ(daif);
	arch_irq_disable();
	return f;
}

void arch_irq_restore(unsigned long flags)
{
	SYSREG_WRITE(daif, flags);
}

/* PSCI is how an ARM system reboots or powers off, and the firmware provides
 * it on every board that matters. */
static u64 psci_call(u32 fn, u64 a0, u64 a1, u64 a2)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = a0;
	register u64 x2 __asm__("x2") = a1;
	register u64 x3 __asm__("x3") = a2;
	__asm__ __volatile__("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

void arch_reboot(void)
{
	psci_call(0x84000009, 0, 0, 0);   /* SYSTEM_RESET */
	arch_halt();
}

void arch_poweroff(void)
{
	psci_call(0x84000008, 0, 0, 0);   /* SYSTEM_OFF */
	arch_halt();
}

size_t arch_hw_random(void *buf, size_t len)
{
	/* FEAT_RNG gives RNDR; most boards do not have it, and the CSPRNG mixes
	 * timing jitter precisely so that this returning zero is survivable. */
	if (!(cpu_feats & RK_FEAT_RDRAND))
		return 0;
	u8 *p = buf;
	size_t done = 0;
	while (done + 8 <= len) {
		u64 v;
		__asm__ __volatile__("mrs %0, s3_3_c2_c4_0" : "=r"(v));
		memcpy(p + done, &v, 8);
		done += 8;
	}
	return done;
}

/* ------------------------------------------------------------- threads */

struct thread *arch_current_thread(void)
{
	return current_thread_slot[arch_cpu_id() % RK_MAX_CPUS];
}

void arch_set_current_thread(struct thread *t)
{
	current_thread_slot[arch_cpu_id() % RK_MAX_CPUS] = t;
}

extern void aarch64_thread_trampoline(void);

void aarch64_thread_entry(void (*entry)(void *), void *arg);

void aarch64_thread_entry(void (*entry)(void *), void *arg)
{
	sched_finish_switch();
	arch_irq_enable();
	entry(arg);
	thread_exit(0);
}

void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg,
                      vaddr_t stack_top)
{
	/* Mirror the layout arch_context_switch pops: six pairs, then the return
	 * address in the x29/x30 pair. */
	u64 *sp = (u64 *)ALIGN_DOWN(stack_top, 16);
	sp -= 12;

	sp[0] = (u64)(uintptr_t)entry;   /* x19 */
	sp[1] = (u64)(uintptr_t)arg;     /* x20 */
	sp[2] = 0; sp[3] = 0;            /* x21, x22 */
	sp[4] = 0; sp[5] = 0;            /* x23, x24 */
	sp[6] = 0; sp[7] = 0;            /* x25, x26 */
	sp[8] = 0; sp[9] = 0;            /* x27, x28 */
	sp[10] = 0;                                            /* x29 */
	sp[11] = (u64)(uintptr_t)aarch64_thread_trampoline;    /* x30 */

	t->sp = sp;
	t->fpu_state = kmalloc_aligned(arch_fpu_state_size(), 16);
}

void arch_thread_free(struct thread *t)
{
	kfree(t->fpu_state);
	t->fpu_state = NULL;
}

size_t arch_fpu_state_size(void) { return 32 * 16 + 16; }   /* v0-v31, fpcr, fpsr */

void arch_fpu_save(struct thread *t)
{
	if (!t || !t->fpu_state)
		return;
	u8 *s = t->fpu_state;
	__asm__ __volatile__(
		"stp q0, q1, [%0, #0]\n\t"   "stp q2, q3, [%0, #32]\n\t"
		"stp q4, q5, [%0, #64]\n\t"  "stp q6, q7, [%0, #96]\n\t"
		"stp q8, q9, [%0, #128]\n\t" "stp q10, q11, [%0, #160]\n\t"
		"stp q12, q13, [%0, #192]\n\t" "stp q14, q15, [%0, #224]\n\t"
		:: "r"(s) : "memory");
}

void arch_fpu_restore(struct thread *t)
{
	if (!t || !t->fpu_state)
		return;
	u8 *s = t->fpu_state;
	__asm__ __volatile__(
		"ldp q0, q1, [%0, #0]\n\t"   "ldp q2, q3, [%0, #32]\n\t"
		"ldp q4, q5, [%0, #64]\n\t"  "ldp q6, q7, [%0, #96]\n\t"
		"ldp q8, q9, [%0, #128]\n\t" "ldp q10, q11, [%0, #160]\n\t"
		"ldp q12, q13, [%0, #192]\n\t" "ldp q14, q15, [%0, #224]\n\t"
		:: "r"(s) : "memory");
}

/* Clean the data cache to the point of unification and invalidate the
 * instruction cache over the same range.
 *
 * The line sizes come from CTR_EL0 rather than being assumed: they differ
 * between implementations, and a loop that steps by the wrong stride either
 * misses lines or wastes a great deal of time. Both fields are log2 of a count
 * of words, hence the shift by two. */
void arch_sync_icache(vaddr_t va, size_t len)
{
	if (!len)
		return;

	u64 ctr = SYSREG_READ(ctr_el0);
	size_t dline = (size_t)4 << ((ctr >> 16) & 0xF);
	size_t iline = (size_t)4 << (ctr & 0xF);
	vaddr_t end = va + len;

	for (vaddr_t p = ALIGN_DOWN(va, dline); p < end; p += dline)
		__asm__ __volatile__("dc cvau, %0" :: "r"(p) : "memory");
	__asm__ __volatile__("dsb ish" ::: "memory");

	for (vaddr_t p = ALIGN_DOWN(va, iline); p < end; p += iline)
		__asm__ __volatile__("ic ivau, %0" :: "r"(p) : "memory");
	__asm__ __volatile__("dsb ish\n\tisb" ::: "memory");
}

int arch_enter_user(vaddr_t entry, vaddr_t stack, void *arg)
{
	struct thread *t = arch_current_thread();
	(void)t;

	/* SP_EL0 is the user stack; the kernel keeps running on SP_EL1, so a trap
	 * back into EL1 lands on the kernel stack without any switching here. */
	SYSREG_WRITE(sp_el0, (u64)stack);
	SYSREG_WRITE(elr_el1, (u64)entry);

	/* SPSR: M[3:0] = 0 selects EL0t, and DAIF clear means the program runs
	 * with interrupts enabled - without which it could never be preempted. */
	SYSREG_WRITE(spsr_el1, 0);

	__asm__ __volatile__(
		"mov x0, %0\n\t"
		"eret"
		:: "r"((u64)(uintptr_t)arg) : "x0", "memory");

	return RK_OK;   /* not reached */
}

/* -------------------------------------------------------------- SMP */

#define PSCI_CPU_ON 0xC4000003u   /* 64-bit calling convention */

/* Handed to a starting core as the PSCI context id. It is read with the MMU
 * off, so it is cache-line aligned and flushed to the point of coherency
 * before the core is released - an uncached read does not see a dirty line. */
static struct ap_boot {
	u64 sp;
	u64 id;
} ap_boot __aligned(64);

static volatile u32 ap_alive;

void aarch64_secondary_start(struct ap_boot *rec);

/* Where a secondary core arrives, in C, with the MMU still off. Nothing may
 * touch unaligned memory until mmu_enable_cpu has returned. */
void aarch64_secondary_start(struct ap_boot *rec)
{
	u32 id = (u32)rec->id;

	mmu_enable_cpu();
	SYSREG_WRITE(tpidr_el1, (u64)id);

	__atomic_store_n(&ap_alive, 1, __ATOMIC_SEQ_CST);

	gic_init_cpu();
	arch_timer_init_cpu();
	if (id + 1 > cpus_online)
		cpus_online = id + 1;

	pr_info("cpu%u online (mpidr %#llx)", id,
	        (unsigned long long)SYSREG_READ(mpidr_el1));

	sched_start_secondary(id);
}

/* PSCI is the only portable way to start a core on ARM, and the affinity value
 * it wants is MPIDR, not an index. Rather than parse /cpus out of the device
 * tree - which several boards get wrong and QEMU sometimes does not provide at
 * all - the cores are asked for in order and the firmware is allowed to say
 * no. INVALID_PARAMETERS means that core does not exist, which is the answer
 * we were looking for. */
int arch_smp_start_secondaries(void)
{
	extern u8 aarch64_secondary_entry[];

	if (rk_cmdline_flag("nosmp"))
		return 0;

	u32 started = 0;
	for (u32 i = 1; i < RK_MAX_CPUS; i++) {
		void *stack = kmalloc_aligned(RK_KSTACK_SIZE, 16);
		if (!stack)
			break;

		ap_boot.sp = (u64)(uintptr_t)stack + RK_KSTACK_SIZE;
		ap_boot.id = i;
		ap_alive   = 0;
		__asm__ __volatile__("dc civac, %0; dsb sy"
		                     :: "r"(&ap_boot) : "memory");

		s64 r = (s64)psci_call(PSCI_CPU_ON, i,
		                       (u64)(uintptr_t)aarch64_secondary_entry,
		                       (u64)(uintptr_t)&ap_boot);
		if (r != 0) {
			kfree(stack);
			break;      /* no such core, or the firmware refused */
		}

		for (int spin = 0; spin < 200 && !ap_alive; spin++)
			rk_mdelay(1);
		if (!ap_alive) {
			pr_warn("cpu%u accepted CPU_ON but never reported in", i);
			break;
		}
		started++;
	}

	if (started)
		pr_info("%u of %u processors started", started + 1, started + 1);
	return (int)started;
}

/* GICv2 software generated interrupts: the target list is a bitmask of CPU
 * interfaces in the top half of the register. */
#define GICD_SGIR 0xF00

void arch_smp_send_ipi(u32 cpu, u32 vec)
{
	u32 sgi = (vec - RK_IPI_RESCHED) & 0xF;
	*(volatile u32 *)(gicd + GICD_SGIR) = ((1u << (cpu & 0xFF)) << 16) | sgi;
}

void arch_smp_broadcast_ipi(u32 vec)
{
	u32 sgi = (vec - RK_IPI_RESCHED) & 0xF;
	*(volatile u32 *)(gicd + GICD_SGIR) = (1u << 24) | sgi;   /* all but self */
}

/* ------------------------------------------------------------------ init */

static void detect_cpu(void)
{
	u64 midr = SYSREG_READ(midr_el1);
	u32 implementer = (u32)((midr >> 24) & 0xFF);
	u32 part = (u32)((midr >> 4) & 0xFFF);

	const char *vendor = implementer == 0x41 ? "ARM"
	                   : implementer == 0x51 ? "Qualcomm"
	                   : implementer == 0x61 ? "Apple"
	                   : implementer == 0x4E ? "NVIDIA"
	                   : implementer == 0x00 ? "QEMU" : "unknown";
	snprintf(cpu_model, sizeof(cpu_model), "%s part %#x", vendor, part);

	u64 pfr0 = SYSREG_READ(id_aa64pfr0_el1);
	if (((pfr0 >> 16) & 0xF) != 0xF) cpu_feats |= RK_FEAT_FPU;
	if (((pfr0 >> 20) & 0xF) != 0xF) cpu_feats |= RK_FEAT_SIMD;

	u64 isar0 = SYSREG_READ(id_aa64isar0_el1);
	if ((isar0 >> 4) & 0xF)  cpu_feats |= RK_FEAT_AES;
	if ((isar0 >> 12) & 0xF) cpu_feats |= RK_FEAT_SHA;
	if ((isar0 >> 44) & 0xF) cpu_feats |= RK_FEAT_DOTPROD;
	if ((isar0 >> 60) & 0xF) cpu_feats |= RK_FEAT_RDRAND;
	cpu_feats |= RK_FEAT_NX | RK_FEAT_INVARTSC;

	pr_info("%s", cpu_model);
}

/* Find the end of RAM by writing to it.
 *
 * Doubling to find an address that fails, then a linear walk back to the last
 * that works. Two things have to be checked at each step, because each catches
 * a different machine: a read-back mismatch catches unpopulated memory that
 * reads as zero, and an aliasing check catches address wrap on boards that
 * decode fewer bits than they expose. A fault is caught by the exception
 * handler and reported through probe_faulted.
 *
 * Only used when the firmware provided no device tree. It is bounded by the
 * eight gigabytes the boot page tables describe.
 */
static u64 probe_ram(paddr_t kernel_end)
{
	const u64 step = 1ull << 20;
	const u64 ceiling = 8ull << 30;

	volatile u64 *anchor = (volatile u64 *)(uintptr_t)RAM_BASE;
	u64 saved_anchor = *anchor;
	*anchor = 0x5245534E544D4E54ull;   /* "RESNTMNT" */

	u64 good = ALIGN_UP(kernel_end - RAM_BASE, step);
	probing = true;

	for (u64 size = good + step; size <= ceiling; size += step) {
		volatile u64 *probe = (volatile u64 *)(uintptr_t)(RAM_BASE + size - 8);

		probe_faulted = false;
		u64 saved = *probe;
		if (probe_faulted)
			break;

		*probe = 0xA5A5A5A5DEADBEEFull ^ size;
		__asm__ __volatile__("dsb sy" ::: "memory");
		if (probe_faulted)
			break;

		bool ok = (*probe == (0xA5A5A5A5DEADBEEFull ^ size)) &&
		          (*anchor == 0x5245534E544D4E54ull);
		*probe = saved;
		if (probe_faulted || !ok)
			break;

		good = size;
	}

	probing = false;
	*anchor = saved_anchor;

	pr_info("no device tree; probed %pB of RAM at %#llx",
	        RK_BYTES(good), (unsigned long long)RAM_BASE);
	return good;
}

void rk_main(struct boot_info *bi) __noreturn;

/* Entered from boot.S with the device tree pointer in x0. */
void aarch64_start(u64 dtb) __noreturn;

void aarch64_start(u64 dtb)
{
	/* The UART first, so that a fault in the next step has somewhere to go.
	 * It is pure aligned MMIO, which is legal even with the MMU off. */
	/* Logical core zero, before anything reads arch_cpu_id(). */
	SYSREG_WRITE(tpidr_el1, 0);

	rk_serial_early_init();

	/* Then the MMU, before any other C runs. With it off every access is
	 * Device-nGnRnE, which forbids unaligned access outright, so ordinary
	 * compiled code takes alignment aborts. This is not an optimisation. */
	mmu_enable();

	rk_serial_console_init();

	extern int rk_fdt_parse(u64 dtb, struct boot_info *out);
	extern u64 rk_fdt_locate(u64 hint, paddr_t base, size_t len);

	/* QEMU booting a bare ELF leaves x0 at zero, and so do several real
	 * boards, so look for the tree rather than assuming a layout. */
	dtb = rk_fdt_locate(dtb, RAM_BASE, 128u << 20);

	extern char __kernel_phys_start[], __kernel_phys_end[];

	if (rk_fdt_parse(dtb, &rk_boot_info) != RK_OK) {
		/* No device tree at all. Rather than hardcode a size and quietly run
		 * with a fraction of the machine, find out how much memory there
		 * actually is. */
		u64 bytes = probe_ram((paddr_t)(uintptr_t)__kernel_phys_end);

		memset(&rk_boot_info, 0, sizeof(rk_boot_info));
		rk_boot_info.magic = RK_BOOT_MAGIC;
		rk_boot_info.version = RK_BOOT_VERSION;
		strlcpy(rk_boot_info.platform, "arm-virt", sizeof(rk_boot_info.platform));
		strlcpy(rk_boot_info.firmware, "none", sizeof(rk_boot_info.firmware));
		rk_boot_info.mmap[0].base = RAM_BASE;
		rk_boot_info.mmap[0].len  = bytes;
		rk_boot_info.mmap[0].type = RK_MEM_USABLE;
		rk_boot_info.mmap_count = 1;
	}

	rk_boot_info.kernel_phys_start = (paddr_t)(uintptr_t)__kernel_phys_start;
	rk_boot_info.kernel_phys_end   = (paddr_t)(uintptr_t)__kernel_phys_end;

	detect_cpu();
	rk_main(&rk_boot_info);
}

void arch_early_init(struct boot_info *bi) { (void)bi; }

void arch_init(struct boot_info *bi)
{
	(void)bi;
	rk_irq_init();
	gic_init();

	/* Start the tick before enabling interrupts. Without it the idle thread
	 * executes wfi and never wakes, which on this architecture is a real
	 * halt rather than the hint it is elsewhere - so the machine stops dead
	 * immediately after "scheduling started" with no further clue. */
	arch_timer_init(RK_HZ);

	arch_irq_enable();
}

void arch_late_init(void) { arch_smp_start_secondaries(); }
