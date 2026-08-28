# Symmetric multiprocessing

RESENTMENT runs on every core the machine has, on all three architectures. This
document is about the two hard parts: getting a second core to execute kernel
code at all, and sharing a scheduler with it without corrupting a stack.

---

## The shape of the problem

A machine powers on with one core running and the rest held in reset or parked
in firmware. Starting them is entirely architecture-specific — three different
mechanisms, none of which resemble each other — but everything *after* the first
instruction of kernel C code is shared. So the split in this kernel is:

```
arch_smp_start_secondaries()      per architecture, in arch/*/
        ↓ (each core, once it has a stack, page tables, traps and a timer)
sched_start_secondary(cpu)        portable, in kernel/sched/sched.c
        ↓
the same run queue every other core is already serving
```

A core that reaches `sched_start_secondary` is indistinguishable from the boot
core. There is no "boot processor" special case in the scheduler.

---

## Starting the cores

### x86_64 — ACPI MADT plus a real-mode trampoline

The hardest of the three, because an x86 core wakes up in **16-bit real mode**
with nothing configured, at a page-aligned address below 1 MiB that the startup
interrupt carries as a single byte.

1. **Discovery.** The ACPI MADT lists every processor's local APIC id.
   A bootloader that speaks Multiboot 2 hands the RSDP over; QEMU's `-kernel`
   path does not, so the kernel falls back to the scan the ACPI specification
   describes — the first kilobyte of the EBDA, then `0xE0000`–`0xFFFFF`,
   sixteen-byte aligned, checksummed.

2. **The trampoline.** `arch/x86_64/ap_trampoline.asm` is a position-independent
   blob copied to physical `0x8000`. It goes 16-bit → 32-bit protected →
   64-bit long mode, loads the boot core's `CR3`, and calls into the kernel at
   its high virtual address. Its parameters — which page table, which stack,
   where to jump, where to report in — are written into the tail of the blob
   before the startup interrupt, because the interrupt itself carries nothing
   but a page number.

3. **The identity map comes back, briefly.** Paging bring-up deliberately drops
   the low identity map, so `x86_paging_identity_low(true)` restores the low
   2 MiB for the duration of start-up and removes it again once every core has
   reported in. A core that has reported in is executing from the high alias and
   never touches a low address again, which is what makes the removal safe.

4. **INIT, then two SIPIs.** The second startup interrupt is what the Intel
   manual asks for and what real hardware sometimes needs; a core that already
   started ignores it. Cores are started one at a time, because the trampoline
   has one parameter block.

**Per-core state.** Each core gets its own GDT, TSS and IST fault stacks. The
GDT could in principle be shared; its TSS descriptor cannot, because a TSS holds
the ring-0 stack pointer and two cores sharing one would take interrupts on each
other's stacks. The hardware agrees: the descriptor's busy bit makes a second
`LTR` against the same descriptor fault. CPU 0's descriptor block is static
because it is set up long before there is a heap; the rest come from the heap.

The IDT *is* shared — it is identical on every core and never changes after boot
— so it is built once and only the `lidt` is per core.

One subtlety worth writing down: reloading the data selectors zeroes `GS`, which
is where the per-CPU block lives. So on an application processor the descriptor
tables are set up **before** `x86_percpu_init`, not after.

### aarch64 — PSCI `CPU_ON`

Firmware does most of the work. `PSCI_CPU_ON(target_mpidr, entry_pa, context)`
starts a core at a physical address with `context` in `x0`.

The context is the address of a two-word record: the stack this core is to use,
and its logical number. It is read with the MMU off, so it is cache-line aligned
and cleaned to the point of coherency (`dc civac`) before the core is released —
an uncached read does not see a dirty line.

The affinity value PSCI wants is MPIDR, not an index. Rather than parse `/cpus`
out of a device tree that several boards get wrong and QEMU sometimes does not
provide at all, cores are asked for in order and the firmware is allowed to say
no: `INVALID_PARAMETERS` means that core does not exist, which is the answer we
were looking for.

The secondary entry stub in `arch/aarch64/boot.S` mirrors the primary one —
drop from EL2 if that is where it landed, MMU and caches off, take the stack,
enable the FPU, install the vector table — and then calls into C, where
`mmu_enable_cpu()` points `TTBR0_EL1` at the tables the boot core already built.

### riscv64 — SBI HSM `hart_start`

The simplest of the three: `sbi_hart_start(hartid, entry_pa, opaque)` starts a
hart in supervisor mode with `a0 = hartid` and `a1 = opaque`. This port runs
with address translation off, so there is nothing else to set up.

**The boot hart is not necessarily hart zero.** OpenSBI picks one, and on QEMU
it regularly picks hart 2 or 3. The entry stub therefore does not test for hart
zero; the first hart to arrive claims the boot role with an `amoswap.w` and the
rest park. The chosen hart id is recorded in `.data` — before `.bss` is zeroed,
which is why it cannot live in `.bss` — and everything downstream reads it: the
SMP code skips it when starting the others, and the PLIC uses **its** supervisor
context (`2N+1`), not hart zero's.

---

## The scheduler on more than one core

### One run queue

There is a single shared ready queue, not per-CPU queues with work stealing.

The trade is deliberate. A shared queue is one lock and is obviously correct,
and it balances perfectly by construction, because any idle core takes the next
runnable thread. Per-CPU queues win on machines with enough cores that the lock
becomes the bottleneck, and they cost a stealing policy, an imbalance metric and
a whole class of subtle bug.

> `ponytail:` global run queue. Split it per-CPU when contention on `rq.lock`
> actually shows up in a profile, not before.

Per-CPU state — the running thread, the idle thread, the idle-time accumulator,
the preemption count, the reschedule flag — is a separate `struct cpu_state`
array beside it.

### The bug this design has to avoid

The natural way to write `__schedule` is:

```c
spin_lock(&rq->lock);
next = pick_next(rq);            /* dequeued under the lock  */
if (prev is still runnable)
        rq_enqueue(rq, prev);    /* ← wrong on a multiprocessor */
spin_unlock(&rq->lock);
context_switch(prev, next);      /* ← prev's stack is still in use here */
```

Between the unlock and the point inside `arch_context_switch` where `prev->sp`
is actually stored, another core can pick `prev` off the queue and start running
on a stack this core has not finished using — with a stale saved stack pointer.
That is a memory-corruption bug that appears as an unrelated page fault, minutes
later, on whichever core happened to lose the race.

The fix is to hand the requeue to the far side of the switch:

```c
cs->prev_pending = prev;         /* owed to the run queue, not yet given back */
...
arch_context_switch(&prev->sp, next->sp);
sched_finish_switch();           /* now it is safe: prev->sp is saved */
```

`sched_finish_switch()` is called in two places, and both are necessary:

- immediately after `arch_context_switch` returns, for a thread that is being
  resumed, and
- as the **first thing** a brand-new thread does, from each architecture's
  thread trampoline — because a new thread never returns into the code that
  took the lock.

The same reasoning is why the run queue lock is released *before* the switch
rather than held across it and dropped by the next thread: a brand-new thread
starts at its trampoline, so the very first switch into one would leave the
queue locked forever.

### Waking a core

Every core takes its own timer tick, so an idle core notices new work within one
scheduling period on its own. One period is up to ten milliseconds, which is a
long time to leave a runnable thread waiting while a core sits halted, so
`sched_wake` sends a reschedule IPI to one idle core. One, not all: waking every
idle core to race for a single thread is how a thundering herd starts.

The IPI is portable at both ends. `arch_smp_send_ipi(cpu, RK_IPI_RESCHED)` goes
out as an APIC interrupt command on x86_64, a GICv2 software generated interrupt
on aarch64, and an SBI IPI on riscv64; each architecture's interrupt path calls
`rk_ipi_handle()`, which is in the scheduler, because what an IPI *means* is a
scheduling question rather than an architecture one.

### Affinity

`sched_set_affinity` is honoured in every picker, not just in the fair one. A
check that is only in some of them makes affinity advisory, which is worse than
not having it. Idle threads are pinned to their own core by construction.

---

## Testing it

```sh
make qemu-test-all
```

builds every architecture and boots each one twice, once single-core and once
with `-smp 4`, running the full 28-assertion shell suite each time. The
multiprocessor runs additionally require the boot log to show every core
reporting in.

Boot logs are ordered even under load: the console write happens inside the log
lock, because two cores logging at once otherwise interleave halfway through a
line, and a boot log that reads as garbage is worse than no boot log.

Eight cores works too, and is worth trying by hand:

```sh
qemu-system-x86_64 -kernel dist/x86_64/resentment32.elf -m 1G -smp 8 -nographic
```

```
[    0.045804] smp      info    ACPI reports 8 usable processors
[    0.061432] smp      info    cpu1 online (apic id 1)
...
[    0.123573] smp      info    cpu7 online (apic id 7)
[    0.125906] smp      info    8 of 8 processors started
```

---

## What is not done

- **No per-CPU run queues.** See the trade above.
- **No NUMA awareness.** Every core is assumed equidistant from memory.
- **No TLB shootdown traffic yet.** `RK_IPI_TLB` is wired end to end and
  handled, but nothing sends it: address spaces are not yet shared across cores
  by a userspace that could unmap under one.
- **CPU hotplug is one-way.** Cores are started at boot and never stopped.
