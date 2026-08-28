# Porting RESENTMENT to a new machine

Porting means implementing `include/rk/arch.h` and a boot stub. Nothing above
`arch/` includes an architecture header, so the portable half — memory
management, scheduling, capabilities, the graph, the AI subsystem, SHE — comes
along unchanged.

There are three ports in the tree to read: `arch/x86_64` is the complete one,
`arch/aarch64` and `arch/riscv64` are smaller and show what the minimum looks
like.

---

## What a port must provide

### Identity

```c
const char *arch_name(void);
const char *arch_cpu_model(void);
u32  arch_cpu_id(void);
u32  arch_cpu_count(void);
u64  arch_cpu_features(void);      /* RK_FEAT_* */
```

The feature bits are not trivia. The AI subsystem picks operator
implementations from them, the crypto layer uses hardware AES and SHA where
they exist, and the scheduler needs to know whether the timestamp counter is
invariant before it trusts a timestamp.

### Execution and interrupts

```c
void arch_halt(void);           /* never returns */
void arch_idle(void);           /* wait for interrupt */
void arch_cpu_relax(void);      /* pause / yield / wfe */
void arch_reboot(void);
void arch_poweroff(void);

void arch_irq_enable(void);
void arch_irq_disable(void);
bool arch_irq_enabled(void);
unsigned long arch_irq_save(void);
void arch_irq_restore(unsigned long flags);
```

`arch_cpu_relax` is on the spinlock path, so it should be the real hint
instruction — `pause`, `yield`, `wfe` — not a no-op.

### Time

```c
u64 arch_cycles(void);
u64 arch_cycles_per_sec(void);
u64 arch_time_ns(void);
s64 arch_wallclock_unix(void);    /* 0 if there is no RTC */
void arch_timer_init(u32 hz);
void arch_timer_oneshot(u64 ns);
```

`arch_time_ns` must use a 128-bit intermediate. At 3 GHz a 64-bit product of
cycles and 10⁹ overflows after six seconds of uptime, which is exactly the kind
of bug that only appears once a machine has been running long enough to matter.

Returning 0 from `arch_wallclock_unix` is fine and honest — the kernel starts
wall time at the epoch and says so in the log.

### Memory

```c
vaddr_t arch_phys_to_virt(paddr_t pa);
paddr_t arch_virt_to_phys(vaddr_t va);

pgtable_t arch_pgtable_kernel(void);
pgtable_t arch_pgtable_create(void);
void      arch_pgtable_destroy(pgtable_t pt);
void      arch_pgtable_switch(pgtable_t pt);

int  arch_map(pgtable_t pt, vaddr_t va, paddr_t pa, size_t len, u32 prot);
int  arch_unmap(pgtable_t pt, vaddr_t va, size_t len);
int  arch_protect(pgtable_t pt, vaddr_t va, size_t len, u32 prot);
bool arch_translate(pgtable_t pt, vaddr_t va, paddr_t *pa, u32 *prot);
void arch_tlb_flush_page(vaddr_t va);
void arch_tlb_flush_all(void);
```

Two requirements that are easy to miss:

**`arch_phys_to_virt` must preserve alignment.** The slab allocator finds a
slab header by masking an object address down to its order-aligned block. The
buddy allocator returns order-aligned physical blocks, and the direct map must
not destroy that. Mapping physical zero at a 2 MiB aligned virtual address is
sufficient.

**`arch_protect` must split large pages.** If the requested range does not
cover a whole huge page, the entry has to be broken into smaller ones first.
Applying the change to the whole huge page instead is how hardening the
kernel's text ends up making its `.bss` read-only, and the machine faults on
the next write. The x86 port has `split_huge()` to copy.

A port may start identity-mapped, as the ARM64 and RISC-V ports currently do:
implement `arch_map` to succeed when `va == pa` and refuse otherwise, rather
than silently ignoring the request.

### Threads

```c
void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg,
                      vaddr_t stack_top);
void arch_context_switch(void **save_sp, void *load_sp);
struct thread *arch_current_thread(void);
void arch_set_current_thread(struct thread *t);
void arch_fpu_save(struct thread *t);
void arch_fpu_restore(struct thread *t);
size_t arch_fpu_state_size(void);
```

`arch_thread_init` builds a stack that `arch_context_switch` can load into,
landing at a trampoline that calls the entry point.

**Get the stack alignment right.** The SysV ABI requires `rsp % 16 == 0`
immediately before a call, so a function sees `rsp % 16 == 8` at its first
instruction. If the trampoline makes a call of its own, the initial frame has
to account for it. Getting this wrong by eight bytes produces a general
protection fault on the first aligned vector instruction the thread executes —
a long way from the cause. The x86 port asserts the invariant:

```c
RK_ASSERT_MSG((((uintptr_t)sp + 7 * 8 + 8) & 15) == 0,
              "thread entry stack alignment is wrong");
```

Start new threads with interrupts **disabled** and enable them in the
trampoline. That keeps the window between "switched in" and "ready to be
interrupted" closed rather than opening it inside the scheduler.

### The interrupt controller

```c
void rk_irq_mask_arch(u32 irq, bool masked);
void rk_irq_eoi_arch(u32 vector);
```

The portable dispatcher in `kernel/core/irq.c` handles shared lines, threaded
handlers, per-line accounting and storm suppression. A port only maps a line
number onto its controller.

### Entropy

```c
size_t arch_hw_random(void *buf, size_t len);   /* may return 0 */
```

Returning zero is fine. The CSPRNG mixes interrupt timing jitter precisely so
that entropy does not depend on the CPU having a hardware generator, which most
ARM and RISC-V boards do not.

---

## The boot stub

Whatever the firmware hands over, turn it into `struct boot_info` and call
`rk_main`. The portable kernel never sees the source format, which is what lets
one design target PCs, phones, boards and microservers.

For a device-tree machine, `kernel/lib/fdt.c` already does the parsing:

```c
void myarch_start(u64 dtb)
{
    rk_serial_early_init();
    rk_serial_console_init();

    if (rk_fdt_parse(dtb, &rk_boot_info) != RK_OK) {
        /* Fall back to something sensible and say so, rather than
         * failing silently on a board whose firmware is unhelpful. */
    }

    rk_boot_info.kernel_phys_start = ...;
    rk_boot_info.kernel_phys_end   = ...;

    rk_main(&rk_boot_info);
}
```

Then three hooks, called in this order by the portable core:

```c
void arch_early_init(struct boot_info *bi);   /* usually empty */
void arch_init(struct boot_info *bi);         /* traps, MMU, IRQ, timer */
void arch_late_init(void);                    /* SMP, syscall entry, hardening */
```

`arch_init` must call `arch_timer_init`. Without a tick the idle thread halts
and never wakes, which looks exactly like a hang in whatever ran last.

Zero `.bss` in the stub. The loader is not required to, and a kernel that only
boots when memory happened to be zero is a kernel that fails in the field.

---

## Wiring it into the build

Add a case to the `Makefile` and to `tools/build-local.sh`:

```make
ifeq ($(ARCH),myarch)
  ARCH_CFLAGS := ...
  AI_CFLAGS   := ...     # the same, plus whatever enables the vector unit
  QEMU        := qemu-system-myarch
  QEMUFLAGS   := -M someboard -m 512M -nographic -kernel $(DIST)/resentment.elf
endif
```

Write `arch/myarch/linker.ld`. Sources are discovered automatically: any `.c`,
`.S` or `.asm` under `arch/$(ARCH)` is compiled.

---

## Checking the port

In order, because each step tells you something different:

1. **`make ARCH=myarch`** — it compiles and links. Undefined symbols name
   exactly which HAL functions are still missing.
2. **`make test`** — the portable half still passes 1440 assertions. This does
   not exercise your port, but it proves you did not break anything shared.
3. **`make ARCH=myarch run`** — serial output. If the banner appears, the boot
   stub, the memory map and the console are right.
4. **The boot self-tests** — if all seven pass, the allocator, crypto, Kaalka,
   graph, capabilities, AI and SHE all work on your machine.
5. **`make qemu-test`** — the shell answers. Everything from the interrupt
   controller to the SHE compiler is working.

The first three architecture-specific bugs are almost always: a physical
address dereferenced after the identity map goes away, a misaligned thread
stack, and a timer that was never started. All three are in this tree's history.
