/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - x86_64 entry and initialisation order.
 *
 * The ordering here is the whole file. Each step depends on the previous one
 * and on nothing later, which is what makes a boot failure land on a line
 * rather than in a mystery.
 */
#include <arch/x86.h>
#include <rk/arch.h>
#include <rk/boot.h>
#include <rk/console.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/panic.h>
#include <rk/mm.h>
#include <rk/irq.h>
#include <rk/time.h>

#undef RK_SUBSYS
#define RK_SUBSYS "x86"

void rk_main(struct boot_info *bi) __noreturn;

#define MB1_BOOTLOADER_MAGIC 0x2BADB002u
#define MB2_BOOTLOADER_MAGIC 0x36D76289u

int x86_multiboot1_parse(u64 info_phys, struct boot_info *out);

/* Entered from boot64.asm with the information pointer in rdi and the loader
 * magic in rsi. */
void x86_start(u64 info_phys, u64 magic) __noreturn;

void x86_start(u64 info_phys, u64 magic)
{
	/* 1. A voice. Everything after this can report its own failure. */
	x86_serial_init();
	rk_serial_console_init();

	/* 2. Turn whichever handover we got into the portable description. */
	int rc = (magic == MB2_BOOTLOADER_MAGIC)
	       ? x86_multiboot2_parse(info_phys, &rk_boot_info)
	       : x86_multiboot1_parse(info_phys, &rk_boot_info);
	if (rc != 0) {
		rk_console_puts("RESENTMENT: unusable boot handover, halting\n");
		arch_halt();
	}

	/* 3. Screen output, now that we know whether there is a framebuffer. */
	rk_console_init(&rk_boot_info);

	/* 4. Traps before anything that can fault. A page fault with no IDT is a
	 *    triple fault and a reboot with no message. */
	x86_gdt_init(0);
	x86_idt_init();
	x86_percpu_init(0, 0);

	/* 5. CPU capabilities, which later steps branch on. */
	x86_cpu_detect();

	/* 6. Interrupt controllers, masked. */
	x86_pic_init();

	/* 7. Physical memory, then page tables, then the heap. */
	rk_main(&rk_boot_info);
}

/* Called by the portable core once mm is up: the parts of arch bring-up that
 * need an allocator. */
void arch_init(struct boot_info *bi)
{
	x86_paging_init(bi);
	rk_irq_init();
	x86_time_init();

	if (x86_apic_init())
		x86_ioapic_init(0, 0);

	/* Start the tick. Nothing above this point needs it, and everything below
	 * does: without a timer the idle thread halts and never wakes, which
	 * looks exactly like a hang in whatever ran last. */
	arch_timer_init(RK_HZ);

	arch_irq_enable();
}

void arch_late_init(void)
{
	extern void x86_serial_irq_init(void);
	extern void x86_paging_harden(void);
	extern void x86_ps2_init(void);

	x86_serial_irq_init();
	x86_ps2_init();
	x86_syscall_init();
	x86_paging_harden();
	x86_smp_init();
}

void arch_early_init(struct boot_info *bi)
{
	(void)bi;   /* already done in x86_start, before the portable core ran */
}
