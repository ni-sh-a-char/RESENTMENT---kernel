# Ring 3

RESENTMENT loads ELF64 programs, builds an address space for each, and runs
them in the least privileged mode the hardware offers. This document covers how
that works, what a program can do from there, and — honestly — which
architectures it works on today.

| Architecture | Loads and runs a program |
|---|---|
| x86_64 | **yes**, in the test suite |
| aarch64 | not yet — see [What is left](#what-is-left) |
| riscv64 | not yet — this port runs with address translation off |

---

## Seeing it

```
resentment> .exec /boot/bin/init

  hello from ring 3.

  pid            1
  uptime         188 ms
  wall clock     1787909138 unix
  entropy        27648
  argc           1
  argv           /boot/bin/init
  syscalls cost  5586541 ns for the above

  Everything above required a working ELF loader, an address space,
  a ring transition and a system call return path. This process holds
  no capabilities: it can compute, print and exit, and nothing else.

[/boot/bin/init exited with 0]
```

Every line there is a claim the kernel had to make good on for the line to
appear: a file read out of the initrd, a header parsed and validated, segments
mapped, a stack built, a privilege transition, and a system call returning a
value the program could use.

`.exec` is a separate verb from `.run` on purpose. `.run` evaluates a SHE
script *inside the kernel*; `.exec` drops to ring 3. They differ in privilege,
not in convenience, and a shell that blurred the two would be teaching the
wrong thing.

---

## The program

`user/src/init.c` is the whole of userspace: one C file and a header with three
inline assembly stubs. It is linked against nothing at all — no libc, no
runtime, no startup object beyond its own `_start`.

That is not minimalism for its own sake. The interesting property of a
capability system is what a program *cannot* do, and a process that holds an
empty capability space and still runs usefully demonstrates it far better than
one that arrives carrying a runtime.

```c
static inline long rk_syscall(long nr, long a0, long a1, long a2)
{
	long ret;
	register long r10 __asm__("r10") = a2;
	__asm__ __volatile__("syscall"
	                     : "=a"(ret)
	                     : "a"(nr), "D"(a0), "S"(a1), "r"(r10)
	                     : "rcx", "r11", "memory");
	return ret;
}
```

**The kernel preserves every register except the return register and whatever
the trap instruction itself destroys** — `rcx` and `r11` on x86_64, nothing on
the other two. That is not a courtesy. A kernel that clobbers the argument
registers corrupts its caller in a way that varies with optimisation level and
is nearly impossible to attribute: this one did, briefly, and the symptom was a
program printing the same wrong number for five different values while the
string literals beside them came out perfectly.

---

## The loader

`kernel/exec/elf.c`. It treats the image as hostile input throughout, because
an ELF header is a structure an attacker fully controls and the classic loader
bugs are all arithmetic:

- an offset plus a length that wraps past the end of the buffer
- a segment claiming to be smaller in the file than in memory, then read as if
  it were not
- a virtual address that lands on kernel memory, or on page zero

Every one is checked, in a first pass over all the headers, **before anything
is mapped** — so a bad segment halfway down cannot leave a half-built address
space behind.

```
validate every header      →  nothing mapped yet, so nothing to unwind
map each segment writable  →  the contents have to be written into it
copy the file-backed part  →  .bss needs nothing; anonymous memory is zeroed
sync the instruction cache →  see below
apply the real protection  →  read-only and executable become true here
build the stack            →  argc, argv, the terminating NULL
enter ring 3               →  never returns except through a trap
```

Two details that are easy to get wrong and expensive to debug:

**The instruction cache.** The loader writes code through a *data* mapping. On
x86 the caches are coherent in hardware and nothing is needed. On ARM and
RISC-V they are not, and the program faults on its first instruction with an
error that says nothing whatsoever about caches. `arch_sync_icache()` exists for
exactly this and is called on every executable segment.

**The stack pointer.** The entry point needs a sixteen-byte aligned stack
pointer *and* `argc` at exactly `[sp]`. Aligning after the pushes satisfies the
first and quietly breaks the second — it slides the pointer down past `argc` and
the program reads padding instead. The padding therefore goes in first, sized
from what is about to be pushed, and an assertion checks the result.

---

## What a program is allowed to do

Nothing it was not handed.

A freshly loaded process has its own address space and an **empty capability
space**. It can compute, it can make the system calls that need no capability —
its own pid, the time, entropy, writing to the debug console — and it can exit.
It cannot open a file, because opening a file means presenting a capability for
a directory, and it holds none.

That is the whole point, and it is why the demo program is not a shell. Handing
a new process a set of capabilities is a policy decision the parent makes, and
the interesting design work is in what that set should be — not in the loader.

---

## What is left

### aarch64

The pieces are in place and individually working: `arch_enter_user` reaches EL0
(confirmed by the saved SPSR on a subsequent trap), the SVC path dispatches
system calls with the arguments read straight out of the saved frame, and a
user address space carries the kernel's mappings so a trap does not unmap the
code servicing it.

What does not work is the last mile: **a freshly mapped user page does not
reliably hold what the loader writes into it.** A byte-at-a-time copy lands, a
sixteen-byte `memcpy` lands, and the full-segment `memcpy` does not — the tail
of it arrives and the head does not. Two real bugs were found and fixed while
chasing it, both of which mattered on their own:

- `arch_map` installed a translation without invalidating the TLB, while
  `arch_unmap` and `arch_protect` both did. A freshly mapped page kept
  resolving through whatever the TLB remembered.
- `prot_to_pte` set `UXN` unconditionally with the comment "kernel mappings are
  never executable from EL0" — correct for kernel pages and fatal for user
  ones, which could then never be executed at all. `PXN` was inverted for the
  same reason: the kernel must never execute user memory, which is what SMEP
  prevents on x86.

A third bug was found while chasing this one and turned out to be unrelated and
much worse: **`sched_tick()` was never called on aarch64 at all.** It was driven
by a test for interrupt line zero in the dispatcher, which is the x86 timer and
nothing else's. So that port had no preemption, no slice accounting, and
`sched_sleep_ms` never returned, because nothing ever woke a sleeper. Nothing in
the suite slept, so it went unnoticed for the life of the port. The dispatcher
now asks the architecture which line drives the scheduler.

None of the three explains the remaining behaviour, so userspace is **off** on
this architecture rather than flaky. `RK_HAVE_USERSPACE` is 0 and `.exec` says so
instead of panicking.

The longer-term fix is almost certainly to stop identity mapping this port and
put the kernel in TTBR1 with userspace in TTBR0, which is the shape the
hardware is designed around. That also removes the reason user programs are
currently linked at 8 GiB rather than at a low address.

### riscv64

This port runs with address translation switched off entirely — `satp` is
never written and `arch_map` returns success only when the virtual address
already equals the physical one. There is no memory protection to put a process
behind, so there is nothing to enable.

Sv39 paging is the prerequisite, and it is a self-contained piece of work worth
doing on its own merits: without it, copy-on-write and demand paging do not
function on RISC-V either.

### Both

- No `SYS_TASK_SPAWN`, so a process cannot start another one; only the kernel
  and the shell can.
- No dynamic linking, and none planned in the kernel. A dynamic linker belongs
  in userspace; putting one here would give the kernel opinions about
  libraries, search paths and symbol versioning that it has no business
  holding. `ET_DYN` images are loaded at a fixed bias and are expected to
  relocate themselves.
- `arch_pgtable_destroy` frees the top-level table and leaks the levels beneath
  it. Harmless while processes are rare and long-lived; not acceptable once
  anything spawns in a loop.
