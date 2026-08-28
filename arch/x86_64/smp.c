/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - SMP discovery.
 *
 * Application processors are discovered from the ACPI MADT and, on a machine
 * without ACPI, from the legacy MP tables. Discovery and start-up are kept
 * separate on purpose: knowing how many cores exist is useful for accounting
 * and for the scheduler even before any of them are running, and bringing a
 * core up is the part that can fail.
 */
#include <arch/x86.h>
#include <rk/arch.h>
#include <rk/boot.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/graph.h>
#include <rk/sched.h>
#include <rk/mm.h>
#include <rk/time.h>
#include <rk/atomic.h>

#undef RK_SUBSYS
#define RK_SUBSYS "smp"

struct acpi_rsdp {
	char magic[8];
	u8   checksum;
	char oem[6];
	u8   revision;
	u32  rsdt;
	/* 2.0 fields follow */
	u32  length;
	u64  xsdt;
	u8   ext_checksum;
	u8   reserved[3];
} __packed;

struct acpi_header {
	char magic[4];
	u32  length;
	u8   revision;
	u8   checksum;
	char oem[6];
	char oem_table[8];
	u32  oem_revision;
	u32  creator;
	u32  creator_revision;
} __packed;

struct madt {
	struct acpi_header hdr;
	u32 lapic_addr;
	u32 flags;
	u8  entries[];
} __packed;

struct madt_entry {
	u8 type;
	u8 length;
} __packed;

struct madt_lapic {
	struct madt_entry e;
	u8  acpi_id;
	u8  apic_id;
	u32 flags;
} __packed;

struct madt_ioapic {
	struct madt_entry e;
	u8  id;
	u8  reserved;
	u32 address;
	u32 gsi_base;
} __packed;

static u32 apic_ids[RK_MAX_CPUS];
static u32 napics;

/* Logical core number to physical APIC id. Built as the cores actually
 * start, rather than assumed to match the order the firmware listed them
 * in. */
static u32 logical_apic[RK_MAX_CPUS];

u32 x86_apic_id_of(u32 cpu)
{
	return cpu < RK_MAX_CPUS ? logical_apic[cpu] : 0;
}

static bool checksum_ok(const void *p, size_t len)
{
	const u8 *b = p;
	u8 sum = 0;
	for (size_t i = 0; i < len; i++)
		sum = (u8)(sum + b[i]);
	return sum == 0;
}

static const struct acpi_header *find_table(paddr_t rsdp_phys, const char *sig)
{
	if (!rsdp_phys)
		return NULL;

	const struct acpi_rsdp *r =
		(const struct acpi_rsdp *)arch_phys_to_virt(rsdp_phys);
	if (memcmp(r->magic, "RSD PTR ", 8) != 0)
		return NULL;

	/* Prefer the XSDT: on a machine with tables above 4 GiB the RSDT entries
	 * are simply not addressable. */
	if (r->revision >= 2 && r->xsdt) {
		const struct acpi_header *x =
			(const struct acpi_header *)arch_phys_to_virt(r->xsdt);
		u32 n = (x->length - sizeof(*x)) / 8;
		const u64 *ptrs = (const u64 *)(x + 1);
		for (u32 i = 0; i < n; i++) {
			const struct acpi_header *t =
				(const struct acpi_header *)arch_phys_to_virt(ptrs[i]);
			if (memcmp(t->magic, sig, 4) == 0)
				return t;
		}
		return NULL;
	}

	const struct acpi_header *rsdt =
		(const struct acpi_header *)arch_phys_to_virt(r->rsdt);
	u32 n = (rsdt->length - sizeof(*rsdt)) / 4;
	const u32 *ptrs = (const u32 *)(rsdt + 1);
	for (u32 i = 0; i < n; i++) {
		const struct acpi_header *t =
			(const struct acpi_header *)arch_phys_to_virt(ptrs[i]);
		if (memcmp(t->magic, sig, 4) == 0)
			return t;
	}
	return NULL;
}

/* A bootloader that speaks Multiboot2 hands the RSDP over directly. QEMU's
 * -kernel path does not, so fall back to the scan the ACPI specification
 * describes: the first kilobyte of the extended BIOS data area, then the BIOS
 * read-only area. Both are 16-byte aligned and both are inside the low 1 MiB
 * this kernel already refuses to allocate from. */
static paddr_t find_rsdp(void)
{
	static const struct { paddr_t base; u64 len; } areas[] = {
		{ 0, 0 },                       /* filled from the EBDA pointer */
		{ 0xE0000, 0x20000 },
	};

	paddr_t ebda = (paddr_t)(*(const u16 *)arch_phys_to_virt(0x40E)) << 4;

	for (unsigned a = 0; a < ARRAY_SIZE(areas); a++) {
		paddr_t base = areas[a].base;
		u64     len  = areas[a].len;
		if (a == 0) {
			if (ebda < 0x80000 || ebda >= 0xA0000)
				continue;
			base = ebda;
			len  = 1024;
		}
		for (u64 off = 0; off + 20 <= len; off += 16) {
			const void *p = (const void *)arch_phys_to_virt(base + off);
			if (memcmp(p, "RSD PTR ", 8) != 0)
				continue;
			/* The first 20 bytes checksum to zero in every revision; a
			 * 2.0 table also checksums over its full length. */
			if (!checksum_ok(p, 20))
				continue;
			const struct acpi_rsdp *r = p;
			if (r->revision >= 2 && !checksum_ok(p, r->length))
				continue;
			return base + off;
		}
	}
	return 0;
}

void x86_acpi_init(struct boot_info *bi)
{
	if (!bi->acpi_rsdp)
		bi->acpi_rsdp = find_rsdp();

	const struct acpi_header *h = find_table(bi->acpi_rsdp, "APIC");
	if (!h) {
		pr_notice("no ACPI MADT; assuming a single processor");
		apic_ids[0] = x86_apic_id();
		napics = 1;
		return;
	}
	if (!checksum_ok(h, h->length)) {
		pr_warn("the MADT checksum is wrong; ignoring it");
		napics = 1;
		return;
	}

	const struct madt *m = (const struct madt *)h;
	const u8 *p = m->entries;
	const u8 *end = (const u8 *)h + h->length;

	while (p + sizeof(struct madt_entry) <= end) {
		const struct madt_entry *e = (const struct madt_entry *)p;
		if (!e->length)
			break;

		if (e->type == 0) {           /* processor local APIC */
			const struct madt_lapic *l = (const struct madt_lapic *)e;
			/* Bit 0 is "enabled"; bit 1 is "online capable". A processor
			 * that is neither must not be counted. */
			if ((l->flags & 1) && napics < RK_MAX_CPUS)
				apic_ids[napics++] = l->apic_id;
		} else if (e->type == 1) {    /* I/O APIC */
			const struct madt_ioapic *io = (const struct madt_ioapic *)e;
			x86_ioapic_init(io->address, io->gsi_base);
		}
		p += e->length;
	}

	pr_info("ACPI reports %u usable processor%s", napics, napics == 1 ? "" : "s");
}

/* ------------------------------------------------------ starting the cores */

/* Where the trampoline is copied. It has to be page aligned and below 1 MiB,
 * because a startup interrupt carries a page number and the core begins in
 * real mode. 0x8000 is conventionally free: below it is the BIOS data area and
 * the interrupt vector table, above it is where bootloaders put themselves and
 * they have finished by now. */
#define TRAMPOLINE_PHYS 0x8000

extern u8 x86_ap_trampoline_start[];
extern u8 x86_ap_trampoline_end[];

/* Offsets of the parameter block at the tail of the trampoline, matching
 * ap_trampoline.asm. Derived rather than hardcoded, so a change to the blob
 * cannot silently desynchronise the two files. */
#define TRAMP_LEN ((size_t)(x86_ap_trampoline_end - x86_ap_trampoline_start))
#define TRAMP_PARAM(n) ((volatile u64 *)(arch_phys_to_virt(TRAMPOLINE_PHYS) + \
                        TRAMP_LEN - (4 - (n)) * 8))
#define TRAMP_CR3   TRAMP_PARAM(0)
#define TRAMP_STACK TRAMP_PARAM(1)
#define TRAMP_ENTRY TRAMP_PARAM(2)
#define TRAMP_ALIVE TRAMP_PARAM(3)

static volatile u32 aps_online;
static volatile u32 aps_reported;
void x86_ap_entry(void);

/* Where an application processor lands, in C, in long mode, on its own stack
 * and sharing the boot processor's page tables. */
void x86_ap_entry(void)
{
	u32 apic = x86_apic_id();
	u32 id = __atomic_add_fetch(&aps_online, 1, __ATOMIC_SEQ_CST);

	/* Descriptor tables first: reloading the data selectors clears GS, which
	 * is where the per-CPU block lives once it is installed. */
	x86_gdt_init(id);
	x86_idt_init();
	x86_percpu_init(id, apic);
	x86_cpu_detect_secondary();
	x86_apic_enable_local();
	x86_apic_timer_init(RK_HZ);
	x86_syscall_init();

	pr_info("cpu%u online (apic id %u)", id, apic);
	x86_set_cpu_online(id + 1);
	__atomic_add_fetch(&aps_reported, 1, __ATOMIC_SEQ_CST);

	/* Joins the same scheduler as every other core: its own idle thread, the
	 * shared run queues, and work stolen from whoever has some. */
	sched_start_secondary(id);
}

int arch_smp_start_secondaries(void)
{
	if (napics <= 1)
		return 0;
	if (rk_cmdline_flag("nosmp")) {
		pr_notice("%u processors present, running on 1 by request (nosmp)",
		          napics);
		return 0;
	}

	if (TRAMP_LEN > 4096) {
		pr_err("the AP trampoline is %llu bytes and will not fit in one page",
		       (unsigned long long)TRAMP_LEN);
		return RK_E2BIG;
	}

	/* The low identity map went away at the end of paging bring-up; a core
	 * starting in real mode needs it back until it reaches the high alias. */
	x86_paging_identity_low(true);

	/* The trampoline page is reserved rather than allocated: it is at a fixed
	 * low address the allocator may not even be tracking. */
	void *dst = (void *)arch_phys_to_virt(TRAMPOLINE_PHYS);
	memcpy(dst, x86_ap_trampoline_start, TRAMP_LEN);

	*TRAMP_CR3   = read_cr3();
	*TRAMP_ENTRY = (u64)(uintptr_t)x86_ap_entry;

	u32 started = 0;
	u32 self = x86_apic_id();
	logical_apic[0] = self;

	for (u32 i = 0; i < napics; i++) {
		if (apic_ids[i] == self)
			continue;

		/* Each core needs its own stack before it can run any C at all. */
		void *stack = kmalloc_aligned(RK_KSTACK_SIZE, 16);
		if (!stack) {
			pr_err("out of memory bringing up apic id %u", apic_ids[i]);
			break;
		}

		*TRAMP_STACK = (u64)(uintptr_t)stack + RK_KSTACK_SIZE;
		*TRAMP_ALIVE = 0;
		smp_mb();

		/* INIT, then two startup interrupts. The second is what the Intel
		 * manual asks for and what real hardware sometimes needs; a core that
		 * already started ignores it. */
		x86_apic_send_init(apic_ids[i]);
		rk_mdelay(10);
		x86_apic_send_startup(apic_ids[i], TRAMPOLINE_PHYS >> 12);
		rk_udelay(200);
		if (!*TRAMP_ALIVE) {
			x86_apic_send_startup(apic_ids[i], TRAMPOLINE_PHYS >> 12);
			rk_udelay(200);
		}

		/* Wait for it to report in rather than assuming. A core that does not
		 * answer is left alone; the machine runs with what it has. */
		for (int spin = 0; spin < 100 && !*TRAMP_ALIVE; spin++)
			rk_mdelay(1);

		if (!*TRAMP_ALIVE) {
			pr_warn("apic id %u did not start", apic_ids[i]);
			kfree(stack);
			continue;
		}
		logical_apic[started + 1] = apic_ids[i];
		started++;

		/* One at a time: the trampoline has one parameter block, so a second
		 * core must not be launched until the first has left it. */
		for (int spin = 0; spin < 100 && aps_online < started; spin++)
			rk_mdelay(1);
	}

	/* Every core that answered is now running from the high alias. Waiting for
	 * the roster to be complete also keeps the boot log in order, which is
	 * worth the handful of microseconds it costs once. */
	for (int spin = 0; spin < 100 && aps_reported < started; spin++)
		rk_mdelay(1);
	x86_paging_identity_low(false);

	pr_info("%u of %u processors started", started + 1, napics);
	return (int)started;
}

void x86_smp_init(void)
{
	x86_acpi_init(&rk_boot_info);
	x86_set_cpu_online(1);
	arch_smp_start_secondaries();
}
