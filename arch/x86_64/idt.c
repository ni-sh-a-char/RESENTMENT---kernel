/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - IDT and trap dispatch.
 */
#include <arch/x86.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/panic.h>
#include <rk/irq.h>
#include <rk/mm.h>
#include <rk/sched.h>
#include <rk/graph.h>
#include <rk/printf.h>
#include <rk/console.h>
#include <rk/errno.h>

#undef RK_SUBSYS
#define RK_SUBSYS "trap"

struct idt_entry {
	u16 offset_lo;
	u16 selector;
	u8  ist;
	u8  type_attr;
	u16 offset_mid;
	u32 offset_hi;
	u32 zero;
} __packed;

struct idt_ptr {
	u16 limit;
	u64 base;
} __packed;

extern u64 x86_isr_table[256];

static struct idt_entry idt[256];
static u64 trap_counts[256];

#define IDT_INTERRUPT 0x8E   /* present, ring 0, interrupt gate: IF cleared */
#define IDT_TRAP      0x8F   /* present, ring 0, trap gate: IF preserved */
#define IDT_USER      0x60   /* DPL 3, OR into the type byte */

#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MCE          3

static const char *const exception_name[32] = {
	"divide error", "debug", "NMI", "breakpoint",
	"overflow", "bound range exceeded", "invalid opcode", "device not available",
	"double fault", "coprocessor overrun", "invalid TSS", "segment not present",
	"stack fault", "general protection", "page fault", "reserved",
	"x87 FPU error", "alignment check", "machine check", "SIMD FP error",
	"virtualisation", "control protection", "reserved", "reserved",
	"reserved", "reserved", "reserved", "hypervisor injection",
	"VMM communication", "security", "reserved", "reserved"
};

static void idt_set(int vec, u64 handler, u8 type, u8 ist)
{
	idt[vec].offset_lo  = (u16)(handler & 0xFFFF);
	idt[vec].selector   = SEL_KCODE;
	idt[vec].ist        = ist & 0x7;
	idt[vec].type_attr  = type;
	idt[vec].offset_mid = (u16)((handler >> 16) & 0xFFFF);
	idt[vec].offset_hi  = (u32)(handler >> 32);
	idt[vec].zero       = 0;
}

/* The table itself is shared: it is identical on every core and never changes
 * after boot. Only the load is per core. */
void x86_idt_init(void)
{
	static bool built;
	if (built)
		goto load;
	built = true;

	memset(idt, 0, sizeof(idt));
	for (int v = 0; v < 256; v++)
		idt_set(v, x86_isr_table[v], IDT_INTERRUPT, 0);

	/* The three faults that must not run on the faulting stack. */
	idt_set(2,  x86_isr_table[2],  IDT_INTERRUPT, IST_NMI);
	idt_set(8,  x86_isr_table[8],  IDT_INTERRUPT, IST_DOUBLE_FAULT);
	idt_set(18, x86_isr_table[18], IDT_INTERRUPT, IST_MCE);

	/* int3 stays reachable from ring 3 so a debugger can breakpoint. */
	idt_set(3, x86_isr_table[3], IDT_TRAP | IDT_USER, 0);

load:;
	struct idt_ptr p = { sizeof(idt) - 1, (u64)(uintptr_t)idt };
	__asm__ __volatile__("lidt %0" :: "m"(p) : "memory");
}

static void dump_frame(struct trapframe *f)
{
	rk_printf("vector %llu (%s) error %#llx\n",
	          (unsigned long long)f->vector,
	          f->vector < 32 ? exception_name[f->vector] : "interrupt",
	          (unsigned long long)f->error);
	rk_printf("rip %#018llx  cs  %#06llx  rflags %#018llx\n",
	          (unsigned long long)f->rip, (unsigned long long)f->cs,
	          (unsigned long long)f->rflags);
	rk_printf("rsp %#018llx  ss  %#06llx  cr2    %#018llx\n",
	          (unsigned long long)f->rsp, (unsigned long long)f->ss,
	          (unsigned long long)read_cr2());
	rk_printf("rax %#018llx  rbx %#018llx  rcx %#018llx\n",
	          (unsigned long long)f->rax, (unsigned long long)f->rbx,
	          (unsigned long long)f->rcx);
	rk_printf("rdx %#018llx  rsi %#018llx  rdi %#018llx\n",
	          (unsigned long long)f->rdx, (unsigned long long)f->rsi,
	          (unsigned long long)f->rdi);
	rk_printf("rbp %#018llx  r8  %#018llx  r9  %#018llx\n",
	          (unsigned long long)f->rbp, (unsigned long long)f->r8,
	          (unsigned long long)f->r9);
}

/* Page fault error code bits, per the SDM. */
#define PF_PRESENT (1u << 0)
#define PF_WRITE   (1u << 1)
#define PF_USER    (1u << 2)
#define PF_RESERVED (1u << 3)
#define PF_EXEC    (1u << 4)

static bool handle_page_fault(struct trapframe *f)
{
	vaddr_t addr = (vaddr_t)read_cr2();
	u32 access = 0;

	if (f->error & PF_WRITE) access |= VM_FAULT_WRITE;
	else if (f->error & PF_EXEC) access |= VM_FAULT_EXEC;
	else access |= VM_FAULT_READ;
	if (f->error & PF_USER) access |= VM_FAULT_USER;

	rk_graph_record(GEV_FAULT, 0, addr, f->error, f->rip);

	struct task *t = task_current();
	struct address_space *as = t ? t->as : as_kernel();
	if (as && vm_handle_fault(as, addr, access) == RK_OK)
		return true;

	/* A fault the VMM cannot resolve in user mode kills the task; in kernel
	 * mode it is a bug and must not be papered over. */
	if (f->error & PF_USER) {
		pr_err("task %llu faulted at %#llx (rip %#llx, err %#llx), terminating",
		       t ? (unsigned long long)t->pid : 0ull,
		       (unsigned long long)addr, (unsigned long long)f->rip,
		       (unsigned long long)f->error);
		if (t)
			task_exit(t, -RK_EFAULT);
		return true;
	}
	return false;
}

void x86_trap_dispatch(struct trapframe *f);

void x86_trap_dispatch(struct trapframe *f)
{
	u64 v = f->vector;
	trap_counts[v & 0xff]++;

	if (v == 14 && handle_page_fault(f))
		return;

	if (v < 32) {
		/* Device-not-available means someone touched the FPU with lazy state
		 * still swapped out. Restoring it is the whole point of the trap. */
		if (v == 7) {
			struct thread *t = arch_current_thread();
			__asm__ __volatile__("clts");
			if (t)
				arch_fpu_restore(t);
			return;
		}
		dump_frame(f);
		panic("unhandled CPU exception %llu (%s)",
		      (unsigned long long)v,
		      exception_name[v]);
	}

	if (v == 0x80 || v == 0x81) {
		/* Legacy software interrupt path, kept for early bring-up before the
		 * SYSCALL MSRs are programmed. */
		extern s64 x86_syscall_from_trap(struct trapframe *f);
		f->rax = (u64)x86_syscall_from_trap(f);
		return;
	}

	/* Inter-processor interrupts do not belong to a device and must not go
	 * through the device dispatch, which would count them as unclaimed and
	 * eventually mask the vector. */
	if (v >= RK_IPI_RESCHED && v <= RK_IPI_CALL) {
		rk_irq_eoi_arch((u32)v);
		rk_ipi_handle((u32)v);
		return;
	}

	rk_irq_dispatch((u32)v);
}

u64 x86_trap_count(u32 vector)
{
	return trap_counts[vector & 0xff];
}
