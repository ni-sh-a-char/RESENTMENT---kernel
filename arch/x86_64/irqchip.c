/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - interrupt controllers.
 *
 * Two generations of hardware, one interface. The 8259 PIC works on every PC
 * ever made and is what a machine looks like at reset; the local APIC and I/O
 * APIC are what a multiprocessor machine actually needs. The kernel brings up
 * the PIC first because it must be masked deliberately rather than left
 * shouting, then moves to the APIC if there is one.
 */
#include <arch/x86.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/irq.h>
#include <rk/time.h>
#include <rk/arch.h>
#include <rk/errno.h>

#undef RK_SUBSYS
#define RK_SUBSYS "irqchip"

/* ------------------------------------------------------------------ 8259 */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

#define IRQ_BASE 32

static u16 pic_mask_shadow = 0xFFFF;
static bool using_apic;

void x86_pic_init(void)
{
	u8 a1 = arch_inb(PIC1_DATA), a2 = arch_inb(PIC2_DATA);
	(void)a1; (void)a2;

	arch_outb(PIC1_CMD, 0x11); arch_io_wait();   /* ICW1: init, expect ICW4 */
	arch_outb(PIC2_CMD, 0x11); arch_io_wait();
	arch_outb(PIC1_DATA, IRQ_BASE);      arch_io_wait();
	arch_outb(PIC2_DATA, IRQ_BASE + 8);  arch_io_wait();
	arch_outb(PIC1_DATA, 0x04); arch_io_wait();  /* slave on IRQ2 */
	arch_outb(PIC2_DATA, 0x02); arch_io_wait();
	arch_outb(PIC1_DATA, 0x01); arch_io_wait();  /* 8086 mode */
	arch_outb(PIC2_DATA, 0x01); arch_io_wait();

	/* Start fully masked. Unmasking is the driver's decision, and a stray
	 * level-triggered line at boot is a very confusing first bug. */
	pic_mask_shadow = 0xFFFF;
	arch_outb(PIC1_DATA, 0xFF);
	arch_outb(PIC2_DATA, 0xFF);
}

void x86_pic_mask(u32 irq, bool masked)
{
	if (irq >= 16)
		return;
	if (masked)
		pic_mask_shadow |= (u16)(1u << irq);
	else
		pic_mask_shadow &= (u16)~(1u << irq);

	/* Unmasking anything on the slave needs the cascade line open. */
	if (irq >= 8 && !masked)
		pic_mask_shadow &= (u16)~(1u << 2);

	arch_outb(PIC1_DATA, (u8)(pic_mask_shadow & 0xFF));
	arch_outb(PIC2_DATA, (u8)(pic_mask_shadow >> 8));
}

void x86_pic_eoi(u32 irq)
{
	if (irq >= 8)
		arch_outb(PIC2_CMD, PIC_EOI);
	arch_outb(PIC1_CMD, PIC_EOI);
}

static void pic_disable(void)
{
	arch_outb(PIC1_DATA, 0xFF);
	arch_outb(PIC2_DATA, 0xFF);
	pic_mask_shadow = 0xFFFF;
}

/* ------------------------------------------------------------ local APIC */

#define LAPIC_ID        0x020
#define LAPIC_VERSION   0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_ICR 0x380
#define LAPIC_TIMER_CCR 0x390
#define LAPIC_TIMER_DIV 0x3E0

#define LAPIC_SW_ENABLE 0x100
#define LVT_MASKED      0x10000
#define LVT_PERIODIC    0x20000

#define VEC_TIMER     0x20   /* same slot the PIT would use */
#define VEC_SPURIOUS  0xFF

static volatile u8 *lapic;

static inline void lapic_write(u32 reg, u32 v) { *(volatile u32 *)(lapic + reg) = v; }
static inline u32  lapic_read(u32 reg)         { return *(volatile u32 *)(lapic + reg); }

bool x86_apic_init(void)
{
	u32 a, b, c, d;
	cpuid_raw(1, 0, &a, &b, &c, &d);
	if (!(d & (1u << 9))) {
		pr_notice("no local APIC, staying on the 8259");
		return false;
	}

	u64 base = rdmsr(MSR_APIC_BASE);
	paddr_t phys = base & 0xFFFFF000ull;
	wrmsr(MSR_APIC_BASE, base | (1ull << 11));   /* global enable */

	lapic = (volatile u8 *)arch_phys_to_virt(phys);

	pic_disable();

	lapic_write(LAPIC_TPR, 0);                                  /* accept all */
	lapic_write(LAPIC_SPURIOUS, LAPIC_SW_ENABLE | VEC_SPURIOUS);
	lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
	lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
	lapic_write(LAPIC_LVT_ERROR, 0xFE);

	using_apic = true;
	pr_info("local APIC at %#llx, id %u, version %#x",
	        (unsigned long long)phys, lapic_read(LAPIC_ID) >> 24,
	        lapic_read(LAPIC_VERSION) & 0xFF);
	return true;
}

void x86_apic_eoi(void)
{
	if (lapic)
		lapic_write(LAPIC_EOI, 0);
}

u32 x86_apic_id(void)
{
	return lapic ? (lapic_read(LAPIC_ID) >> 24) : 0;
}

/* The APIC timer counts at the bus clock, whose rate is not architecturally
 * discoverable, so calibrate it against the PIT once and reuse the result. */
static u32 apic_ticks_per_ms;

static void apic_timer_calibrate(void)
{
	lapic_write(LAPIC_TIMER_DIV, 0x3);          /* divide by 16 */
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

	rk_udelay(10000);                            /* 10 ms via the PIT/TSC */

	u32 elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_TIMER_CCR);
	lapic_write(LAPIC_TIMER_ICR, 0);
	apic_ticks_per_ms = elapsed / 10;
	if (!apic_ticks_per_ms)
		apic_ticks_per_ms = 1;
	pr_info("APIC timer: %u ticks/ms", apic_ticks_per_ms);
}

void x86_apic_timer_init(u32 hz)
{
	if (!lapic)
		return;
	if (!apic_ticks_per_ms)
		apic_timer_calibrate();

	u32 count = (apic_ticks_per_ms * 1000) / (hz ? hz : 100);
	lapic_write(LAPIC_TIMER_DIV, 0x3);
	lapic_write(LAPIC_LVT_TIMER, VEC_TIMER | LVT_PERIODIC);
	lapic_write(LAPIC_TIMER_ICR, count ? count : 1);
}

/* Enable the local APIC on a processor other than the boot one. Everything
 * global was already configured; this is only the per-core part. */
void x86_apic_enable_local(void)
{
	if (!lapic)
		return;
	wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | (1ull << 11));
	lapic_write(LAPIC_TPR, 0);
	lapic_write(LAPIC_SPURIOUS, LAPIC_SW_ENABLE | VEC_SPURIOUS);
	lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
	lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
	lapic_write(LAPIC_LVT_ERROR, 0xFE);
}

/* The two halves of waking a core. INIT resets it into a wait-for-startup
 * state; the startup interrupt tells it which page to begin executing at.
 * Both must be sent from the boot processor and both are asynchronous, which
 * is why the caller waits for the core to report in rather than for these. */
void x86_apic_send_init(u32 apic_id)
{
	if (!lapic)
		return;
	lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
	/* Delivery mode 101 (INIT), level assert, edge trigger. */
	lapic_write(LAPIC_ICR_LOW, (5 << 8) | (1 << 14));
	while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
		arch_cpu_relax();
}

void x86_apic_send_startup(u32 apic_id, u8 page)
{
	if (!lapic)
		return;
	lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
	/* Delivery mode 110 (startup); the vector field carries the page. */
	lapic_write(LAPIC_ICR_LOW, page | (6 << 8) | (1 << 14));
	while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
		arch_cpu_relax();
}

void arch_smp_send_ipi(u32 cpu, u32 vector)
{
	if (!lapic)
		return;
	/* The interrupt command register addresses a physical APIC id, which is
	 * not the same number as the logical core the scheduler thinks in. */
	lapic_write(LAPIC_ICR_HIGH, x86_apic_id_of(cpu) << 24);
	lapic_write(LAPIC_ICR_LOW, vector | (0 << 8) | (1 << 14));
	while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
		arch_cpu_relax();
}

void arch_smp_broadcast_ipi(u32 vector)
{
	if (!lapic)
		return;
	/* Shorthand 0b11: all excluding self. */
	lapic_write(LAPIC_ICR_LOW, vector | (1 << 14) | (3 << 18));
	while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
		arch_cpu_relax();
}

/* --------------------------------------------------------------- I/O APIC */

#define IOAPIC_REGSEL 0x00
#define IOAPIC_IOWIN  0x10
#define IOAPIC_ID     0x00
#define IOAPIC_VER    0x01
#define IOAPIC_REDTBL 0x10

static volatile u8 *ioapic;
static u32 ioapic_gsi_base;
static u32 ioapic_pins;

static u32 ioapic_read(u32 reg)
{
	*(volatile u32 *)(ioapic + IOAPIC_REGSEL) = reg;
	return *(volatile u32 *)(ioapic + IOAPIC_IOWIN);
}

static void ioapic_write(u32 reg, u32 v)
{
	*(volatile u32 *)(ioapic + IOAPIC_REGSEL) = reg;
	*(volatile u32 *)(ioapic + IOAPIC_IOWIN) = v;
}

void x86_ioapic_init(paddr_t phys, u32 gsi_base)
{
	if (!phys)
		phys = 0xFEC00000;
	ioapic = (volatile u8 *)arch_phys_to_virt(phys);
	ioapic_gsi_base = gsi_base;
	ioapic_pins = ((ioapic_read(IOAPIC_VER) >> 16) & 0xFF) + 1;

	/* Mask every pin. Routing is established when a driver asks for a line. */
	for (u32 i = 0; i < ioapic_pins; i++) {
		ioapic_write(IOAPIC_REDTBL + i * 2, LVT_MASKED | (IRQ_BASE + i));
		ioapic_write(IOAPIC_REDTBL + i * 2 + 1, 0);
	}
	pr_info("I/O APIC at %#llx, %u pins, gsi base %u",
	        (unsigned long long)phys, ioapic_pins, gsi_base);
}

static void ioapic_route(u32 gsi, u8 vector, u32 cpu, bool masked)
{
	if (!ioapic || gsi < ioapic_gsi_base || gsi - ioapic_gsi_base >= ioapic_pins)
		return;
	u32 pin = gsi - ioapic_gsi_base;
	ioapic_write(IOAPIC_REDTBL + pin * 2 + 1, cpu << 24);
	ioapic_write(IOAPIC_REDTBL + pin * 2, (masked ? LVT_MASKED : 0) | vector);
}

/* -------------------------------------------------------- portable facade */

void rk_irq_mask_arch(u32 irq, bool masked)
{
	if (using_apic && ioapic)
		ioapic_route(irq, (u8)(IRQ_BASE + irq), 0, masked);
	else
		x86_pic_mask(irq, masked);
}

void rk_irq_eoi_arch(u32 vector)
{
	if (using_apic)
		x86_apic_eoi();
	else if (vector >= IRQ_BASE && vector < IRQ_BASE + 16)
		x86_pic_eoi(vector - IRQ_BASE);
}

bool x86_using_apic(void) { return using_apic; }
