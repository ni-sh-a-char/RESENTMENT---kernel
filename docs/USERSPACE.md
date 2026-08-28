# Ring 3

RESENTMENT loads ELF64 programs, builds an address space for each, and runs
them in the least privileged mode the hardware offers — on **every**
architecture it supports.

| Architecture | Ring 3 | Paging | How the kernel is mapped |
|---|---|---|---|
| x86_64 | ✅ | 4-level | higher half at −2 GiB, user gets the whole lower canonical half |
| aarch64 | ✅ | 39-bit VA, 4 KiB granule | identity through TTBR0; user above the bottom 8 GiB |
| riscv64 | ✅ | Sv39 | identity; user above the bottom 8 GiB, below the canonical ceiling |

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

## What it took

Every architecture got here through a different bug, and each one was invisible
until something finally exercised the path.

### aarch64 — a NEON `memcpy` and an exception that does not save NEON

A freshly mapped page did not reliably hold what the loader wrote into it. A
single store landed, a sixteen-byte copy landed, and a full-segment copy lost
exactly 32 bytes at the head of every page it touched.

32 bytes is `stp q0, q1` — a NEON pair store. `kernel/lib/string.c` is compiled
with `-mgeneral-regs-only`, which is supposed to mean no floating point and no
SIMD anywhere in the kernel. **clang does not honour it for the loop
vectoriser**, so `memcpy` contained NEON regardless. When a page fault landed on
that store, the handler ran C code, the retry re-executed the store, and by then
`q0`/`q1` held whatever the handler had left there — because the exception path
does not save the SIMD registers, on the entirely reasonable assumption that
nothing outside `kernel/ai/` uses them.

`-mno-implicit-float` closes the gap. The design was right; the compiler was not
being made to follow it.

Two more bugs surfaced on the way and were worth fixing on their own:
`arch_map` installed translations without invalidating the TLB while `arch_unmap`
and `arch_protect` both did, and `prot_to_pte` set `UXN` unconditionally — so a
user page could never be executed at all.

### riscv64 — no MMU at all, then two more

This port ran with address translation switched off. `arch_map` succeeded only
where the virtual address already equalled the physical one, so demand paging
and copy-on-write did not function either. It now has Sv39: three levels,
identity-mapped through the bottom eight gigabytes with gigapage leaves, exactly
as the aarch64 port is.

Then two things that only a multiprocessor or a user program would ever show:

**`satp` is per hart.** Secondary harts never enabled paging, so they ran in
bare mode while the boot hart translated — writing straight through a virtual
address as though it were physical, silently, and only under `-smp`.

**Sv39 requires a canonical address.** Bits 63:39 must all equal bit 38. The
user stack had been placed just under 512 GiB, which sets bit 38 with zeros
above it: not unmapped, *malformed*, and the hardware faults on it however
correct the page tables are. Userspace therefore stops just short of 2^38.
aarch64 has no such constraint, because TTBR0 covers a range rather than a
signed half — which is why the same layout worked there and not here.

### All three — a window for touching user memory

Every one of these architectures can forbid the kernel from dereferencing a
user pointer, and by default does: SMAP on x86, `sstatus.SUM` on RISC-V, PAN on
ARM. That is a good default. But the loader writes segments to the addresses
the linker chose, and those are user addresses.

So there is now `arch_user_access_begin()` / `arch_user_access_end()`, used by
the bounded copy helpers and by the loader, and nowhere else. Preemption is
disabled inside it, because the permission lives in a per-CPU register that no
context switch on this kernel saves — a thread preempted inside the window
would leave the door open for whatever ran next.

On RISC-V, forgetting it does not fail gracefully: the store faults, the fault
handler maps a page that is already mapped, returns success, and the
instruction retries and faults again, for ever.

---

## What is still not done

- **No `SYS_TASK_SPAWN`**, so a process cannot start another one; only the
  kernel and the shell can.
- **No dynamic linking**, and none planned in the kernel. A dynamic linker
  belongs in userspace; putting one here would give the kernel opinions about
  libraries, search paths and symbol versioning that it has no business
  holding. `ET_DYN` images are loaded at a fixed bias and are expected to
  relocate themselves.
- **`arch_pgtable_destroy` frees the top-level table and leaks the levels
  beneath it.** Harmless while processes are rare and long-lived; not
  acceptable once anything spawns in a loop.
- **No TLB shootdown.** `RK_IPI_TLB` is wired end to end and handled, but
  nothing sends it. It becomes necessary the moment two cores share an address
  space that one of them unmaps.
