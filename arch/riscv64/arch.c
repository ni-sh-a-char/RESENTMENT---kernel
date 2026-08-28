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

void riscv64_trap(u64 cause, u64 epc, u64 tval);

void riscv64_trap(u64 cause, u64 epc, u64 tval)
{
	bool is_interrupt = (cause >> 63) != 0;
	u64 code = cause & 0x7FFFFFFFFFFFFFFFull;

	if (is_interrupt) {
		switch (code) {
		case 5:   /* supervisor timer */
			schedule_next_tick();
			rk_timer_tick();
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

	if (code == 8 || code == 9) {   /* ecall from user or supervisor */
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

/* Identity mapped with Sv39 paging left off, as on the aarch64 port. Every
 * caller above arch/ already goes through arch_map, so enabling paging is
 * confined to these functions.
 * ponytail: identity map, no Sv39. */
vaddr_t arch_phys_to_virt(paddr_t pa) { return (vaddr_t)pa; }
paddr_t arch_virt_to_phys(vaddr_t va) { return (paddr_t)va; }

pgtable_t arch_pgtable_kernel(void) { return NULL; }
pgtable_t arch_pgtable_create(void) { return NULL; }
void arch_pgtable_destroy(pgtable_t p) { (void)p; }
void arch_pgtable_switch(pgtable_t p) { (void)p; }

int arch_map(pgtable_t pt, vaddr_t va, paddr_t pa, size_t len, u32 prot)
{
	(void)pt; (void)len; (void)prot;
	return (va == (vaddr_t)pa) ? RK_OK : RK_ENOTSUP;
}

int arch_unmap(pgtable_t pt, vaddr_t va, size_t len)
{ (void)pt; (void)va; (void)len; return RK_OK; }
int arch_protect(pgtable_t pt, vaddr_t va, size_t len, u32 prot)
{ (void)pt; (void)va; (void)len; (void)prot; return RK_OK; }

bool arch_translate(pgtable_t pt, vaddr_t va, paddr_t *pa, u32 *prot)
{
	(void)pt;
	if (pa)   *pa = (paddr_t)va;
	if (prot) *prot = RK_PROT_READ | RK_PROT_WRITE | RK_PROT_EXEC;
	return true;
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

int arch_enter_user(vaddr_t entry, vaddr_t stack, void *arg)
{ (void)entry; (void)stack; (void)arg; return RK_ENOSYS; }

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
