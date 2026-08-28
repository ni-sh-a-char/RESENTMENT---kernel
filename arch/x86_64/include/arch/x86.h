/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - x86_64 private definitions.
 *
 * Nothing above arch/ may include this file.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>

#define KERNEL_VMA      0xFFFFFFFF80000000ull
#define DIRECT_MAP_BASE 0xFFFF800000000000ull
#define DIRECT_MAP_SIZE 0x0000000100000000ull   /* 4 GiB mapped by the boot stub */
#define RECURSIVE_SLOT  510ull
#define RECURSIVE_BASE  0xFFFFFF0000000000ull

/* Segment selectors, matching the order in gdt.c. */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x1b   /* ring 3 */
#define SEL_UCODE 0x23   /* ring 3 */
#define SEL_TSS   0x28

/* ------------------------------------------------------------ port I/O */

static inline void arch_outb(u16 port, u8 v)  { __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void arch_outw(u16 port, u16 v) { __asm__ __volatile__("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline void arch_outl(u16 port, u32 v) { __asm__ __volatile__("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8   arch_inb(u16 port)  { u8 v;  __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16  arch_inw(u16 port)  { u16 v; __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32  arch_inl(u16 port)  { u32 v; __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void arch_io_wait(void)  { arch_outb(0x80, 0); }

/* -------------------------------------------------------------- msr / cr */

static inline u64 rdmsr(u32 msr)
{
	u32 lo, hi;
	__asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((u64)hi << 32) | lo;
}

static inline void wrmsr(u32 msr, u64 v)
{
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}

static inline u64 read_cr0(void) { u64 v; __asm__ __volatile__("mov %%cr0, %0" : "=r"(v)); return v; }
static inline u64 read_cr2(void) { u64 v; __asm__ __volatile__("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); return v; }
static inline u64 read_cr4(void) { u64 v; __asm__ __volatile__("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(u64 v) { __asm__ __volatile__("mov %0, %%cr0" :: "r"(v) : "memory"); }
static inline void write_cr3(u64 v) { __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline void write_cr4(u64 v) { __asm__ __volatile__("mov %0, %%cr4" :: "r"(v) : "memory"); }

#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084
#define MSR_FS_BASE     0xC0000100
#define MSR_GS_BASE     0xC0000101
#define MSR_KGS_BASE    0xC0000102
#define MSR_APIC_BASE   0x0000001B
#define MSR_TSC_DEADLINE 0x000006E0

static inline void cpuid_raw(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ __volatile__("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
	                     : "a"(leaf), "c"(sub));
}

static inline u64 rdtsc(void)
{
	u32 lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

/* Serialising read, for measuring intervals rather than sampling a clock. */
static inline u64 rdtscp(void)
{
	u32 lo, hi, aux;
	__asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
	return ((u64)hi << 32) | lo;
}

/* ------------------------------------------------------------ trap frame */

/* Pushed by the ISR stubs. The order here is the order in isr.asm; changing
 * one without the other is the classic way to spend an evening. */
struct trapframe {
	u64 r15, r14, r13, r12, r11, r10, r9, r8;
	u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
	u64 vector, error;
	u64 rip, cs, rflags, rsp, ss;
};

struct thread;
struct boot_info;

void x86_gdt_init(u32 cpu);
void x86_idt_init(void);
void x86_tss_set_kernel_stack(vaddr_t sp);
void x86_pic_init(void);
void x86_pic_mask(u32 irq, bool masked);
void x86_pic_eoi(u32 irq);
bool x86_apic_init(void);
void x86_apic_eoi(void);
void x86_apic_timer_init(u32 hz);
void x86_apic_enable_local(void);
void x86_apic_send_init(u32 apic_id);
void x86_apic_send_startup(u32 apic_id, u8 page);
void x86_cpu_detect_secondary(void);
void x86_paging_identity_low(bool on);
u32  x86_apic_id_of(u32 cpu);
void x86_pit_init(u32 hz);
u64  x86_pit_calibrate_tsc(void);
void x86_cpu_detect(void);
void x86_paging_init(struct boot_info *bi);
void x86_syscall_init(void);
void x86_smp_init(void);
void x86_percpu_init(u32 id, u32 apic_id);
void x86_set_cpu_online(u32 n);
void x86_serial_irq_init(void);
void x86_paging_harden(void);
void x86_trap_dispatch(struct trapframe *f);
void x86_serial_init(void);
int  x86_multiboot2_parse(u64 info_phys, struct boot_info *out);

void x86_time_init(void);
void x86_ioapic_init(paddr_t phys, u32 gsi_base);
bool x86_using_apic(void);
u32  x86_apic_id(void);
u64  x86_pit_ticks(void);
u64  x86_trap_count(u32 vector);
void x86_acpi_init(struct boot_info *bi);

extern u64 x86_tsc_hz;
