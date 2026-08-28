/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - riscv64 support.
 *
 * RISC-V puts more of the machine behind firmware than the other two
 * architectures: SBI handles the timer, the console of last resort, reset and
 * bringing up secondary harts. That makes this the smallest of the three ports
 * and the one most likely to work on a board nobody has tested.
 *
 * Targets the QEMU virt machine, which is also what SiFive and StarFive boards
 * look like: an NS16550 UART, a PLIC, and SBI underneath.
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
#define RK_SUBSYS "riscv64"

#define UART0_BASE 0x10000000ul
#define PLIC_BASE  0x0C000000ul
#define TIMEBASE   10000000ull      /* the virt machine counts at 10 MHz */

/* Set by the entry stub before .bss is zeroed, so it lives in .data. */
extern u64 boot_hart_id;

static char cpu_model[64] = "riscv64";
static u64  cpu_feats;
static u64  timer_freq = TIMEBASE;
static struct thread *current_thread_slot[RK_MAX_CPUS];
static u32  cpus_online = 1;

const char *arch_name(void) { return "riscv64"; }
const char *arch_cpu_model(void) { return cpu_model; }

/* tp holds the logical core number. There is no supervisor-readable hart id
 * CSR, so it has to be carried somewhere, and tp is free in a kernel with no
 * thread-local storage. */
u32  arch_cpu_id(void)
{
	u64 v;
	__asm__ __volatile__("mv %0, tp" : "=r"(v));
	return (u32)v;
}
u32  arch_cpu_count(void) { return cpus_online; }
u64  arch_cpu_features(void) { return cpu_feats; }

/* ------------------------------------------------------------------ SBI */

/* The Supervisor Binary Interface: an ecall with the extension id in a7 and
 * the function id in a6. Every RISC-V system has it, which is why this port
 * needs no board-specific reset or timer code. */
struct sbiret { long error; long value; };

static struct sbiret sbi_call(long eid, long fid, long a0, long a1, long a2)
{
	register long r_a0 __asm__("a0") = a0;
	register long r_a1 __asm__("a1") = a1;
	register long r_a2 __asm__("a2") = a2;
	register long r_a6 __asm__("a6") = fid;
	register long r_a7 __asm__("a7") = eid;

	__asm__ __volatile__("ecall"
	                     : "+r"(r_a0), "+r"(r_a1)
	                     : "r"(r_a2), "r"(r_a6), "r"(r_a7)
	                     : "memory");
	return (struct sbiret){ r_a0, r_a1 };
}

#define SBI_SET_TIMER      0x54494D45
#define SBI_HSM            0x48534D    /* hart state management */
#define SBI_IPI            0x735049
#define SBI_SYSTEM_RESET   0x53525354
#define SBI_CONSOLE_PUTCHAR 0x01

/* ---------------------------------------------------------------- UART */

static volatile u8 *uart = (volatile u8 *)UART0_BASE;

#define UART_THR 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_LSR 5

void rk_serial_putc(char c)
{
	for (int i = 0; i < 100000; i++)
		if (uart[UART_LSR] & 0x20)
			break;
	uart[UART_THR] = (u8)c;
}

static void ns16550_write(struct rk_console *con, const char *s, size_t n)
{
	(void)con;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\n')
			rk_serial_putc('\r');
		rk_serial_putc(s[i]);
	}
}

static void ns16550_putc(struct rk_console *c, char ch) { ns16550_write(c, &ch, 1); }

static const u8 ansi[16] = { 30, 34, 32, 36, 31, 35, 33, 37, 90, 94, 92, 96, 91, 95, 93, 97 };

static void ns16550_color(struct rk_console *c, u8 fg, u8 bg)
{
	(void)c; (void)bg;
	char buf[12];
	int n = snprintf(buf, sizeof(buf), "\033[%um", ansi[fg & 0xF]);
	ns16550_write(NULL, buf, (size_t)n);
}

static void ns16550_clear(struct rk_console *c)
{
	(void)c;
	ns16550_write(NULL, "\033[2J\033[H", 7);
}

/* Polled receive; see the x86 port for why this exists alongside the
 * interrupt path. */
static int ns16550_getc(struct rk_console *c)
{
	(void)c;
	if (!(uart[UART_LSR] & 0x01))
		return -1;
	return (int)uart[UART_THR];
}

static struct rk_console uart_console = {
	.name = "ttyS0",
	.putc = ns16550_putc,
	.write = ns16550_write,
	.clear = ns16550_clear,
	.set_color = ns16550_color,
	.getc = ns16550_getc,
	.width = 80,
	.height = 25,
};

void rk_serial_early_init(void)
{
	uart[UART_IER] = 0x00;
	uart[UART_LCR] = 0x80;      /* divisor latch */
	uart[0] = 0x03;             /* 38400 from the virt machine clock */
	uart[1] = 0x00;
	uart[UART_LCR] = 0x03;      /* 8N1 */
	uart[UART_FCR] = 0x07;      /* FIFO on, cleared */
}

void rk_serial_console_init(void) { rk_console_register(&uart_console); }

/* ---------------------------------------------------------------- PLIC */

/* The PLIC has one context per hart per privilege level, laid out in hart
 * order with machine mode first. The supervisor context of hart N is
 * therefore 2N+1 - and N is whichever hart the firmware chose to boot, not
 * necessarily zero. */
#define PLIC_PRIORITY(i)  (PLIC_BASE + (i) * 4)
#define PLIC_CONTEXT      (2ul * boot_hart_id + 1ul)
#define PLIC_ENABLE       (PLIC_BASE + 0x2000 + 0x80 * PLIC_CONTEXT)
#define PLIC_THRESHOLD    (PLIC_BASE + 0x200000 + 0x1000 * PLIC_CONTEXT)
#define PLIC_CLAIM        (PLIC_THRESHOLD + 4)

static void plic_init(void)
{
	/* Every source at a usable priority but disabled, and a threshold of zero
	 * so that anything enabled will actually be delivered. */
	for (u32 i = 1; i < 128; i++)
		*(volatile u32 *)PLIC_PRIORITY(i) = 1;
	for (u32 i = 0; i < 4; i++)
		*(volatile u32 *)(PLIC_ENABLE + i * 4) = 0;
	*(volatile u32 *)PLIC_THRESHOLD = 0;
}

void rk_irq_mask_arch(u32 irq, bool masked)
{
	if (!irq || irq >= 128)
		return;
	volatile u32 *reg = (volatile u32 *)(PLIC_ENABLE + (irq / 32) * 4);
	if (masked)
		*reg &= ~(1u << (irq % 32));
	else
		*reg |= 1u << (irq % 32);
}

void rk_irq_eoi_arch(u32 vector)
{
	if (vector && vector < 128)
		*(volatile u32 *)PLIC_CLAIM = vector;
}

/* ---------------------------------------------------------------- traps */

#define CSR_READ(name) \
	({ u64 __v; __asm__ __volatile__("csrr %0, " #name : "=r"(__v)); __v; })
#define CSR_WRITE(name, v) \
	__asm__ __volatile__("csrw " #name ", %0" :: "r"((u64)(v)))
#define CSR_SET(name, v) \
	__asm__ __volatile__("csrs " #name ", %0" :: "r"((u64)(v)))
#define CSR_CLEAR(name, v) \
	__asm__ __volatile__("csrc " #name ", %0" :: "r"((u64)(v)))

static u64 timer_interval = TIMEBASE / 100;

static void schedule_next_tick(void)
{
	u64 now = CSR_READ(time);
	sbi_call(SBI_SET_TIMER, 0, (long)(now + timer_interval), 0, 0);
}

void riscv64_trap(u64 cause, u64 epc, u64 tval, u64 *frame);

void riscv64_trap(u64 cause, u64 epc, u64 tval, u64 *frame)
{
	bool is_interrupt = (cause >> 63) != 0;
	u64 code = cause & 0x7FFFFFFFFFFFFFFFull;

	if (is_interrupt) {
		switch (code) {
		case 5:   /* supervisor timer */
			schedule_next_tick();
			rk_timer_tick();
			/* This timer is a trap and never reaches rk_irq_dispatch, so the
			 * scheduler is driven from here rather than from the line the
			 * dispatcher was told about. Without it there is no preemption
			 * and nothing ever wakes a sleeping thread. */
			if (sched_active())
				sched_tick();
			return;
		case 9: { /* supervisor external, via the PLIC */
			u32 id = *(volatile u32 *)PLIC_CLAIM;
			if (id)
				rk_irq_dispatch(id);
			return;
		}
		case 1:   /* supervisor software, used for IPIs */
			CSR_CLEAR(sip, 1 << 1);
			rk_ipi_handle(RK_IPI_RESCHED);
			return;
		default:
			return;
		}
	}

	/* Page faults: 12 instruction, 13 load, 15 store. */
	if (code == 12 || code == 13 || code == 15) {
		u32 access = code == 12 ? VM_FAULT_EXEC
		           : code == 13 ? VM_FAULT_READ : VM_FAULT_WRITE;
		struct task *t = task_current();
		struct address_space *as = t ? t->as : as_kernel();
		if (as && vm_handle_fault(as, (vaddr_t)tval, access) == RK_OK)
			return;
	}

	if (code == 8) {   /* ecall from user mode: a system call */
		/* The frame is what trap_vector wrote: a0..a7 at words 8..15 and
		 * sepc at word 16. sepc points *at* the ecall, so it has to be
		 * stepped past or the program issues it again forever. */
		struct rk_syscall_args a = {
			.nr  = frame[15],
			.a0  = frame[8],  .a1 = frame[9],  .a2 = frame[10],
			.a3  = frame[11], .a4 = frame[12], .a5 = frame[13],
		};
		frame[8]   = (u64)rk_syscall_dispatch(&a);
		frame[16] += 4;
		return;
	}
	if (code == 9) {   /* ecall from supervisor: nothing issues one */
		frame[16] += 4;
		return;
	}

	panic("unhandled trap: cause %llu epc %#llx tval %#llx",
	      (unsigned long long)code, (unsigned long long)epc,
	      (unsigned long long)tval);
}

/* ---------------------------------------------------------------- timer */

void arch_timer_init(u32 hz)
{
	timer_interval = timer_freq / (hz ? hz : 100);
	schedule_next_tick();
	pr_info("SBI timer at %llu Hz, ticking at %u Hz",
	        (unsigned long long)timer_freq, hz);
}

void arch_timer_oneshot(u64 ns)
{
	u64 ticks = (u64)(((__uint128_t)ns * timer_freq) / 1000000000ull);
	sbi_call(SBI_SET_TIMER, 0, (long)(CSR_READ(time) + ticks), 0, 0);
}

u64 arch_cycles(void) { return CSR_READ(time); }
u64 arch_cycles_per_sec(void) { return timer_freq; }

u64 arch_time_ns(void)
{
	return (u64)(((__uint128_t)arch_cycles() * 1000000000ull) / timer_freq);
}

/* ------------------------------------------------------------------ RTC */

/* The goldfish RTC, which is what QEMU's virt machine provides and what a
 * surprising number of ARM and RISC-V boards emulate. Two registers: reading
 * the low word latches the high one, and the pair is nanoseconds since the
 * Unix epoch.
 *
 * The base address is the virt machine's. Probing for it properly means
 * walking the device tree for "google,goldfish-rtc", and this port cannot rely
 * on having a tree at all - QEMU hands none to a bare ELF. So the address is
 * assumed and the *answer* is checked instead: a time outside a plausible
 * window is treated as no clock rather than as a clock that is wrong, because
 * Kaalka would otherwise derive epoch keys from nonsense.
 *
 * Without this the wall clock is zero, seals have no real time base, and every
 * clock angle Kaalka reads is zero as well - which is exactly the failure that
 * looks like everything working. */
#define GOLDFISH_RTC_BASE  0x00101000ul
#define GOLDFISH_TIME_LOW  0x00
#define GOLDFISH_TIME_HIGH 0x04

/* 2020-01-01 and 2100-01-01. Wide enough not to reject a badly set clock,
 * narrow enough to reject a register that is not an RTC. */
#define WALLCLOCK_MIN 1577836800ll
#define WALLCLOCK_MAX 4102444800ll

s64 arch_wallclock_unix(void)
{
	volatile const u32 *rtc = (volatile const u32 *)GOLDFISH_RTC_BASE;

	u32 lo = rtc[GOLDFISH_TIME_LOW / 4];
	u32 hi = rtc[GOLDFISH_TIME_HIGH / 4];
	s64 secs = (s64)(((u64)hi << 32 | lo) / 1000000000ull);

	return (secs >= WALLCLOCK_MIN && secs < WALLCLOCK_MAX) ? secs : 0;
}

/* --------------------------------------------------------------- memory */

/* Sv39: three levels of 512 entries, 39 bits of virtual address.
 *
 *   VA[38:30]  index into the root      1 GiB per entry
 *   VA[29:21]  index into level one     2 MiB per entry
 *   VA[20:12]  index into level zero    4 KiB per entry
 *
 * The kernel is identity mapped through the bottom eight gigabytes using
 * gigapage leaves in the root, exactly as the aarch64 port is: everything
 * below one gigabyte is the device region on this machine and everything above
 * is RAM, so eight entries describe the whole machine and every pointer the
 * kernel already holds stays valid when translation is switched on.
 *
 * That leaves entries 8 through 511 - eight gigabytes to five hundred and
 * twelve - free for user address spaces, which is why user programs are linked
 * above 8 GiB here rather than at a traditional low address.
 */

#define PTE_V   (1ull << 0)
#define PTE_R   (1ull << 1)
#define PTE_W   (1ull << 2)
#define PTE_X   (1ull << 3)
#define PTE_U   (1ull << 4)
#define PTE_G   (1ull << 5)
#define PTE_A   (1ull << 6)
#define PTE_D   (1ull << 7)
#define PTE_RWX (PTE_R | PTE_W | PTE_X)

#define SATP_SV39 (8ull << 60)

#define PTE_PPN(pte)  (((pte) >> 10) << 12)
#define PPN_PTE(pa)   ((((u64)(pa)) >> 12) << 10)
#define VPN(va, lvl)  (((u64)(va) >> (12 + 9 * (lvl))) & 0x1FF)

/* The root table for the kernel. Static because it has to exist before there
 * is an allocator to take it from. */
static u64 boot_root[512] __aligned(4096);
static u64 *kernel_root;
static bool paging_on;

vaddr_t arch_phys_to_virt(paddr_t pa) { return (vaddr_t)pa; }
paddr_t arch_virt_to_phys(vaddr_t va) { return (paddr_t)va; }

pgtable_t arch_pgtable_kernel(void) { return (pgtable_t)kernel_root; }

static u64 prot_to_pte(u32 prot)
{
	u64 e = PTE_V | PTE_A | PTE_D;
	if (prot & RK_PROT_READ)  e |= PTE_R;
	if (prot & RK_PROT_WRITE) e |= PTE_W;
	if (prot & RK_PROT_EXEC)  e |= PTE_X;
	if (prot & RK_PROT_USER)  e |= PTE_U;
	else                      e |= PTE_G;
	/* A leaf with no permissions at all is a pointer to the next level, not a
	 * mapping, so a page that grants nothing has to be spelled read-only. */
	if (!(e & PTE_RWX))
		e |= PTE_R;
	return e;
}

/* Walk to the next level, optionally creating it. Returns the table, not the
 * entry, so the caller indexes it itself. */
static u64 *walk(u64 *table, u64 idx, bool create)
{
	u64 pte = table[idx];

	if (pte & PTE_V) {
		if (pte & PTE_RWX)
			return NULL;         /* a leaf where a table was wanted */
		return (u64 *)(uintptr_t)PTE_PPN(pte);
	}
	if (!create)
		return NULL;

	paddr_t pa = pmm_alloc_page();
	if (!pa)
		return NULL;
	memset((void *)(uintptr_t)pa, 0, RK_PAGE_SIZE);
	table[idx] = PPN_PTE(pa) | PTE_V;
	return (u64 *)(uintptr_t)pa;
}

static u64 *leaf_of(u64 *root, vaddr_t va, bool create)
{
	u64 *l1 = walk(root, VPN(va, 2), create);
	if (!l1)
		return NULL;
	u64 *l0 = walk(l1, VPN(va, 1), create);
	if (!l0)
		return NULL;
	return &l0[VPN(va, 0)];
}

pgtable_t arch_pgtable_create(void)
{
	paddr_t pa = pmm_alloc_page();
	if (!pa)
		return NULL;

	u64 *root = (u64 *)(uintptr_t)pa;
	memset(root, 0, RK_PAGE_SIZE);

	/* Every address space carries the kernel's mappings, or the first trap
	 * out of a user program would find no handler mapped to trap into. */
	if (kernel_root)
		for (size_t i = 0; i < 8; i++)
			root[i] = kernel_root[i];

	return (pgtable_t)(uintptr_t)pa;
}

void arch_pgtable_destroy(pgtable_t p)
{
	/* Frees the root only. The levels beneath it leak, which is acceptable
	 * while processes are rare and long lived and is not once anything
	 * spawns in a loop.
	 * ponytail: walk and free the whole tree when that day comes. */
	if (p && (u64 *)p != kernel_root)
		pmm_free_page((paddr_t)(uintptr_t)p);
}

void arch_pgtable_switch(pgtable_t p)
{
	if (!p || !paging_on)
		return;
	u64 satp = SATP_SV39 | ((u64)(uintptr_t)p >> 12);
	__asm__ __volatile__("sfence.vma\n\tcsrw satp, %0\n\tsfence.vma"
	                     :: "r"(satp) : "memory");
}

int arch_map(pgtable_t pt, vaddr_t va, paddr_t pa, size_t len, u32 prot)
{
	u64 *root = pt ? (u64 *)pt : kernel_root;
	if (!root)
		return (va == (vaddr_t)pa) ? RK_OK : RK_ENOTSUP;
	if (!IS_ALIGNED(va, RK_PAGE_SIZE) || !IS_ALIGNED(pa, RK_PAGE_SIZE))
		return RK_EINVAL;

	u64 flags = prot_to_pte(prot);

	for (size_t off = 0; off < len; off += RK_PAGE_SIZE) {
		u64 *leaf = leaf_of(root, va + off, true);
		if (!leaf)
			return RK_ENOMEM;
		*leaf = PPN_PTE(pa + off) | flags;
		arch_tlb_flush_page(va + off);
	}
	return RK_OK;
}

int arch_unmap(pgtable_t pt, vaddr_t va, size_t len)
{
	u64 *root = pt ? (u64 *)pt : kernel_root;
	if (!root)
		return RK_OK;

	for (size_t off = 0; off < len; off += RK_PAGE_SIZE) {
		u64 *leaf = leaf_of(root, va + off, false);
		if (leaf) {
			*leaf = 0;
			arch_tlb_flush_page(va + off);
		}
	}
	return RK_OK;
}

int arch_protect(pgtable_t pt, vaddr_t va, size_t len, u32 prot)
{
	u64 *root = pt ? (u64 *)pt : kernel_root;
	if (!root)
		return RK_OK;

	u64 flags = prot_to_pte(prot);
	for (size_t off = 0; off < len; off += RK_PAGE_SIZE) {
		u64 *leaf = leaf_of(root, va + off, false);
		if (leaf && (*leaf & PTE_V)) {
			*leaf = (*leaf & ~0x3FFull) | flags;
			arch_tlb_flush_page(va + off);
		}
	}
	return RK_OK;
}

bool arch_translate(pgtable_t pt, vaddr_t va, paddr_t *pa, u32 *prot)
{
	u64 *root = pt ? (u64 *)pt : kernel_root;
	if (!root) {
		if (pa)   *pa = (paddr_t)va;
		if (prot) *prot = RK_PROT_READ | RK_PROT_WRITE | RK_PROT_EXEC;
		return true;
	}

	/* A gigapage in the root is how the kernel is mapped, so the walk has to
	 * stop at a leaf wherever it finds one. */
	u64 pte = root[VPN(va, 2)];
	if (!(pte & PTE_V))
		return false;
	u64 size = 1ull << 30;
	if (!(pte & PTE_RWX)) {
		u64 *l1 = (u64 *)(uintptr_t)PTE_PPN(pte);
		pte = l1[VPN(va, 1)];
		if (!(pte & PTE_V))
			return false;
		size = 1ull << 21;
		if (!(pte & PTE_RWX)) {
			u64 *l0 = (u64 *)(uintptr_t)PTE_PPN(pte);
			pte = l0[VPN(va, 0)];
			if (!(pte & PTE_V))
				return false;
			size = 1ull << 12;
		}
	}

	if (pa)
		*pa = PTE_PPN(pte) + (va & (size - 1));
	if (prot) {
		*prot = 0;
		if (pte & PTE_R) *prot |= RK_PROT_READ;
		if (pte & PTE_W) *prot |= RK_PROT_WRITE;
		if (pte & PTE_X) *prot |= RK_PROT_EXEC;
		if (pte & PTE_U) *prot |= RK_PROT_USER;
	}
	return true;
}

/* Point this hart at the page tables the boot hart built. satp is per hart, so
 * a secondary that skips this runs in bare mode while everything else
 * translates - it would write straight through a virtual address as though it
 * were physical, silently, and only on a multiprocessor. */
static void mmu_enable_cpu(void)
{
	if (!kernel_root)
		return;
	u64 satp = SATP_SV39 | ((u64)(uintptr_t)kernel_root >> 12);
	__asm__ __volatile__("sfence.vma\n\tcsrw satp, %0\n\tsfence.vma"
	                     :: "r"(satp) : "memory");
}

/* Build the identity map and switch translation on. Everything the kernel
 * holds is a physical address and stays one, so nothing observable changes -
 * which is the point: the port keeps working exactly as it did, and now has
 * page tables to hang demand paging and user address spaces off. */
static void mmu_enable(void)
{
	for (size_t i = 0; i < 8; i++)
		boot_root[i] = PPN_PTE((u64)i << 30) |
		               PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_G;

	kernel_root = boot_root;
	paging_on = true;

	u64 satp = SATP_SV39 | ((u64)(uintptr_t)boot_root >> 12);
	__asm__ __volatile__("sfence.vma\n\tcsrw satp, %0\n\tsfence.vma"
	                     :: "r"(satp) : "memory");

	pr_info("Sv39 paging on, %pB identity mapped, root at %#llx",
	        RK_BYTES(8ull << 30), (unsigned long long)(uintptr_t)boot_root);
}

void arch_tlb_flush_page(vaddr_t va)
{
	__asm__ __volatile__("sfence.vma %0, zero" :: "r"(va) : "memory");
}

void arch_tlb_flush_all(void)
{
	__asm__ __volatile__("sfence.vma" ::: "memory");
}

/* ------------------------------------------------------------ execution */

void arch_halt(void)
{
	arch_irq_disable();
	for (;;)
		__asm__ __volatile__("wfi");
}

void arch_idle(void)
{
	CSR_SET(sstatus, 1 << 1);
	__asm__ __volatile__("wfi");
}

void arch_cpu_relax(void) { __asm__ __volatile__("nop"); }

void arch_irq_enable(void)  { CSR_SET(sstatus, 1 << 1); }
void arch_irq_disable(void) { CSR_CLEAR(sstatus, 1 << 1); }
bool arch_irq_enabled(void) { return (CSR_READ(sstatus) & (1 << 1)) != 0; }

unsigned long arch_irq_save(void)
{
	unsigned long f = (unsigned long)CSR_READ(sstatus);
	arch_irq_disable();
	return f;
}

void arch_irq_restore(unsigned long flags)
{
	if (flags & (1 << 1))
		arch_irq_enable();
}

void arch_reboot(void)
{
	sbi_call(SBI_SYSTEM_RESET, 0, 1, 0, 0);   /* cold reboot */
	arch_halt();
}

void arch_poweroff(void)
{
	sbi_call(SBI_SYSTEM_RESET, 0, 0, 0, 0);   /* shutdown */
	arch_halt();
}

/* Zkr gives a seed CSR, but almost no hardware has it yet. Returning zero is
 * correct and the CSPRNG compensates with timing jitter. */
size_t arch_hw_random(void *buf, size_t len) { (void)buf; (void)len; return 0; }

/* ------------------------------------------------------------- threads */

struct thread *arch_current_thread(void)
{
	return current_thread_slot[arch_cpu_id() % RK_MAX_CPUS];
}

void arch_set_current_thread(struct thread *t)
{
	current_thread_slot[arch_cpu_id() % RK_MAX_CPUS] = t;
}

extern void riscv64_thread_trampoline(void);

void riscv64_thread_entry(void (*entry)(void *), void *arg);

void riscv64_thread_entry(void (*entry)(void *), void *arg)
{
	sched_finish_switch();
	arch_irq_enable();
	entry(arg);
	thread_exit(0);
}

void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg,
                      vaddr_t stack_top)
{
	/* Mirror what arch_context_switch pops: ra then s0 through s11. */
	u64 *sp = (u64 *)ALIGN_DOWN(stack_top, 16);
	sp -= 14;

	sp[0] = (u64)(uintptr_t)riscv64_thread_trampoline;   /* ra */
	sp[1] = 0;                                           /* s0 */
	sp[2] = (u64)(uintptr_t)entry;                       /* s1 */
	sp[3] = (u64)(uintptr_t)arg;                         /* s2 */
	for (int i = 4; i < 14; i++)
		sp[i] = 0;

	t->sp = sp;
	t->fpu_state = kmalloc_aligned(arch_fpu_state_size(), 16);
}

void arch_thread_free(struct thread *t)
{
	kfree(t->fpu_state);
	t->fpu_state = NULL;
}

size_t arch_fpu_state_size(void) { return 32 * 8 + 8; }

/* The base kernel is compiled without the F and D extensions, so there is no
 * float state to save except in the AI subsystem, which brackets its own use. */
void arch_fpu_save(struct thread *t) { (void)t; }
void arch_fpu_restore(struct thread *t) { (void)t; }

/* RISC-V has one instruction for this and it applies to the whole hart, so the
 * range is ignored. On a multiprocessor the other harts need one too before
 * they execute the new code; nothing here writes code that another hart runs,
 * so that is not yet arranged. */
void arch_sync_icache(vaddr_t va, size_t len)
{
	(void)va;
	(void)len;
	__asm__ __volatile__("fence.i" ::: "memory");
}

/* sstatus.SUM - permit Supervisor User Memory access. Clear by default, so
 * S-mode faults on any page whose PTE has the U bit. */
#define SSTATUS_SUM (1ull << 18)

static u32 user_access_depth[RK_MAX_CPUS];

void arch_user_access_begin(void)
{
	sched_preempt_disable();
	u32 cpu = arch_cpu_id() % RK_MAX_CPUS;
	if (user_access_depth[cpu]++ == 0)
		CSR_SET(sstatus, SSTATUS_SUM);
}

void arch_user_access_end(void)
{
	u32 cpu = arch_cpu_id() % RK_MAX_CPUS;
	if (user_access_depth[cpu] && --user_access_depth[cpu] == 0)
		CSR_CLEAR(sstatus, SSTATUS_SUM);
	sched_preempt_enable();
}

int arch_enter_user(vaddr_t entry, vaddr_t stack, void *arg)
{
	struct thread *t = arch_current_thread();

	/* The kernel stack this hart returns to on the next trap. trap_vector
	 * swaps it out of sscratch, which is the whole mechanism keeping kernel
	 * code off a stack the user program chose. */
	if (t && t->kstack)
		CSR_WRITE(sscratch, t->kstack + t->kstack_size);

	/* SPP clear means sret drops to user mode. SPIE set means interrupts are
	 * enabled once it gets there, without which it could never be preempted. */
	u64 status = CSR_READ(sstatus);
	status &= ~(1ull << 8);        /* SPP  = 0 */
	status |=  (1ull << 5);        /* SPIE = 1 */
	CSR_WRITE(sstatus, status);
	CSR_WRITE(sepc, (u64)entry);

	__asm__ __volatile__(
		"mv   sp, %0\n\t"
		"mv   a0, %1\n\t"
		"sret"
		:: "r"((u64)stack), "r"((u64)(uintptr_t)arg)
		: "memory");

	return RK_OK;   /* not reached */
}

/* -------------------------------------------------------------- SMP */

static struct ap_boot {
	u64 sp;
	u64 id;
} ap_boot __aligned(64);

/* Logical core number to hart id. They are only equal when the firmware
 * happened to boot hart zero. */
static u64 hart_of[RK_MAX_CPUS];

static volatile u32 ap_alive;

void riscv64_secondary_start(u64 id);

void riscv64_secondary_start(u64 id)
{
	__atomic_store_n(&ap_alive, 1, __ATOMIC_SEQ_CST);

	mmu_enable_cpu();                 /* satp is per hart */
	CSR_SET(sstatus, 1ull << 13);     /* the float unit, as on the boot hart */
	schedule_next_tick();

	if (id + 1 > cpus_online)
		cpus_online = (u32)id + 1;

	pr_info("cpu%llu online (hart %llu)", (unsigned long long)id,
	        (unsigned long long)hart_of[id % RK_MAX_CPUS]);

	sched_start_secondary((u32)id);
}

/* SBI hart state management. Harts are asked for in order and the firmware is
 * allowed to say no: INVALID_PARAM means that hart does not exist, which is
 * how the count is discovered without trusting a device tree. The boot hart is
 * hart 0 because the entry stub refuses to continue on any other. */
int arch_smp_start_secondaries(void)
{
	extern u8 riscv64_secondary_entry[];

	if (rk_cmdline_flag("nosmp"))
		return 0;

	hart_of[0] = boot_hart_id;

	u32 started = 0;
	for (u64 hart = 0; hart < RK_MAX_CPUS; hart++) {
		if (hart == boot_hart_id)
			continue;

		void *stack = kmalloc_aligned(RK_KSTACK_SIZE, 16);
		if (!stack)
			break;

		u32 id = started + 1;
		hart_of[id] = hart;      /* before the start: the core reads it */
		ap_boot.sp = (u64)(uintptr_t)stack + RK_KSTACK_SIZE;
		ap_boot.id = id;
		ap_alive   = 0;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);

		struct sbiret r = sbi_call(SBI_HSM, 0, (long)hart,
		                           (long)(uintptr_t)riscv64_secondary_entry,
		                           (long)(uintptr_t)&ap_boot);
		if (r.error != 0) {
			kfree(stack);
			break;
		}

		for (int spin = 0; spin < 200 && !ap_alive; spin++)
			rk_mdelay(1);
		if (!ap_alive) {
			pr_warn("hart %llu accepted hart_start but never reported in",
			        (unsigned long long)hart);
			break;
		}

		started++;
	}

	if (started)
		pr_info("%u of %u processors started", started + 1, started + 1);
	return (int)started;
}
/* RISC-V has exactly one software interrupt, so every IPI kind arrives as a
 * reschedule. That is the only kind this kernel sends. */
void arch_smp_send_ipi(u32 cpu, u32 vec)
{
	(void)vec;
	u64 hart = hart_of[cpu % RK_MAX_CPUS];
	sbi_call(SBI_IPI, 0, 1L << (hart & 63), (long)(hart & ~63ull), 0);
}

void arch_smp_broadcast_ipi(u32 vec)
{
	(void)vec;
	sbi_call(SBI_IPI, 0, 0, -1L, 0);   /* every hart */
}

/* ------------------------------------------------------------------ init */

void rk_main(struct boot_info *bi) __noreturn;
void riscv64_start(u64 dtb) __noreturn;

void riscv64_start(u64 dtb)
{
	rk_serial_early_init();
	rk_serial_console_init();

	extern int rk_fdt_parse(u64 dtb, struct boot_info *out);
	if (rk_fdt_parse(dtb, &rk_boot_info) != RK_OK) {
		memset(&rk_boot_info, 0, sizeof(rk_boot_info));
		rk_boot_info.magic = RK_BOOT_MAGIC;
		rk_boot_info.version = RK_BOOT_VERSION;
		strlcpy(rk_boot_info.platform, "qemu-virt", sizeof(rk_boot_info.platform));
		strlcpy(rk_boot_info.firmware, "sbi", sizeof(rk_boot_info.firmware));
		rk_boot_info.mmap[0].base = 0x80000000;
		rk_boot_info.mmap[0].len  = 128ull << 20;
		rk_boot_info.mmap[0].type = RK_MEM_USABLE;
		rk_boot_info.mmap_count = 1;
		rk_console_puts("no device tree; assuming the QEMU virt layout\n");
	}

	extern char __kernel_phys_start[], __kernel_phys_end[];
	rk_boot_info.kernel_phys_start = (paddr_t)(uintptr_t)__kernel_phys_start;
	rk_boot_info.kernel_phys_end   = (paddr_t)(uintptr_t)__kernel_phys_end;

	strlcpy(cpu_model, "RISC-V rv64imac supervisor", sizeof(cpu_model));
	cpu_feats = RK_FEAT_NX | RK_FEAT_INVARTSC;

	rk_main(&rk_boot_info);
}

void arch_early_init(struct boot_info *bi) { (void)bi; }

void arch_init(struct boot_info *bi)
{
	(void)bi;
	rk_irq_init();
	mmu_enable();
	plic_init();

	/* Mark the float unit as usable once, at boot. The base kernel is built
	 * without the F and D extensions, so the only float is in the AI
	 * subsystem, which disables preemption around it and therefore cannot
	 * lose the state to a context switch. */
	CSR_SET(sstatus, 1ull << 13);

	arch_timer_init(RK_HZ);
	arch_irq_enable();
}

void arch_late_init(void) { arch_smp_start_secondaries(); }
