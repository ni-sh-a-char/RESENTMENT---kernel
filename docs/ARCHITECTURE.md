# RESENTMENT architecture

This document explains how the kernel is put together and, more usefully, why
each piece is shaped the way it is. Where a decision was a trade, the trade is
named.

---

## The three claims

RESENTMENT exists because three things are true of modern systems and badly
served by conventional kernel design.

**Permissions outlive their purpose.** A capability granted for a task that
finished last week is still valid. Revocation is a sweep somebody has to run,
which means it is a sweep somebody forgets to run. If authority carried an
expiry the way a certificate does, the default behaviour of a forgotten
permission would be to stop working.

**System state is not addressable.** Asking "is this machine in the state I
attested" means scraping the text output of a dozen tools and hoping. There is
no name for the state of a machine, so there is nothing to compare, nothing to
sign, and nothing to reproduce.

**Inference is scheduled by a library that cannot see the machine.** A
userspace runtime cannot preempt a half-finished decode for a latency-critical
one, cannot share loaded weights between processes, cannot page an attention
cache under memory pressure, and cannot prevent a model reading memory it was
never granted. All four are kernel jobs.

Each claim produces one subsystem: capabilities with Kaalka seals, the runtime
graph, and the AI subsystem with its own scheduling class.

---

## Layering

```
                      SHE scripts, agents, native binaries
                                     |
   +---------------------------------+---------------------------------+
   |                          system call layer                        |
   |  every call takes a capability; none grants authority from nothing |
   +---------------------------------+---------------------------------+
                                     |
   +----------+----------+-----------+-----------+----------+----------+
   |   cap    |   ipc    |    vfs    |    ai     |   she    |  graph   |
   | seals,   | endpoint | ramfs     | tensors   | compiler | merkle   |
   | derive,  | channel  | devfs     | ops       | vm       | events   |
   | revoke   | notify   | graphfs   | kv cache  | stdlib   | memfab   |
   +----------+----------+-----------+-----------+----------+----------+
                                     |
   +----------+----------+-----------+-----------+----------+----------+
   |    mm    |  sched   |   sync    |  crypto   |  device  |  time    |
   |  buddy   | 4 classes| mutex     | sha256    | bus/drv  | timers   |
   |  slab    | deadline | rwlock    | chacha20  | binding  | replay   |
   |  vmspace | waitq    | condvar   | kaalka    |          | clock    |
   +----------+----------+-----------+-----------+----------+----------+
                                     |
   +-------------------------------------------------------------------+
   |                    arch HAL  (include/rk/arch.h)                   |
   |        x86_64            aarch64             riscv64               |
   +-------------------------------------------------------------------+
```

The rule that keeps this honest: **nothing above `arch/` includes an
architecture header.** There is exactly one exception, `vga.c`, which is legacy
PC hardware by definition and compiles to nothing elsewhere; the exception is
noted in the file.

---

## Boot

The order in `kernel/core/main.c` is a dependency order. Each step needs the
ones above it and nothing below, which is what makes a boot failure land on a
line rather than in a mystery.

```
arch entry           serial console first, so everything after can report failure
  parse firmware     multiboot2 / device tree -> one portable struct boot_info
  console            framebuffer or VGA, once we know which exists
  traps              GDT/IDT before anything that can fault
  cpu features       later steps branch on these
  interrupt ctrl     masked; unmasking is each driver's decision
rk_main
  1  pmm             the arch layer needs an allocator to finish page tables
  2  arch_init       paging, IRQ dispatch, timers, APIC
  3  mm_init         slab heap, address spaces
  4  time            everything below timestamps against it
  5  crypto          entropy, then known-answer self-tests
  6  kaalka          needs entropy and the clock
  7  graph + memfab  needs the heap and Kaalka for sealing
  8  capabilities    Kaalka-sealed, graph-visible
  9  scheduler       needs capabilities to build the kernel task
 10  ipc, vfs, syscall
 11  arch_late_init  SMP, syscall MSRs, image hardening
     init thread     devices, filesystems, AI, self-tests, then the shell
     sched_start     becomes the idle thread
```

The one ordering subtlety worth knowing: `pmm_init` runs *before* `arch_init`,
because extending the direct map past the 4 GiB the boot stub built needs page
tables, and allocating page tables needs a page allocator.

---

## Memory

Three layers, each chosen for a reason that shows up in practice.

**Buddy allocator for physical frames.** Not a bitmap: DMA and accelerators
need physically contiguous memory, and finding a contiguous 2 MiB run in a
bitmap is an O(n) scan that gets slower as the machine fills. A buddy makes it
O(log n) and makes coalescing automatic.

**Slab caches for the heap.** The kernel allocates the same few object sizes
over and over — a thread, a vnode, a graph node, a capability — and a general
allocator spends its life re-deriving that fact. Every `kmalloc` carries a
16-byte header, which costs memory and buys freeing without being told the
size, double-free detection, and an honest `ksize`. In a kernel that ships a
scripting language and a tensor runtime, both allocating on behalf of untrusted
callers, that trade is not close.

**vm_objects under address spaces.** Making the *object* the unit of storage
rather than the mapping is why fork, shared memory, file mapping and zero-copy
IPC are one code path: they differ only in who else holds a reference. Pages
are allocated on fault, so a tensor arena of a gigabyte costs nothing until it
is touched.

The slab layer depends on one thing worth stating: `arch_phys_to_virt` must
preserve alignment, because a slab header is found by masking an object address
down to its order-aligned block. Every supported architecture maps physical
zero at a 2 MiB aligned virtual address. The host test harness had to be fixed
to match, which is how the assumption got documented.

---

## Scheduling

Four classes, checked strictly in order:

| Class | Policy | Why it exists |
|---|---|---|
| `SCHED_REALTIME` | earliest deadline first, admission controlled | hard deadlines |
| `SCHED_INFERENCE` | EDF with a per-period budget, demoted on overrun | see below |
| `SCHED_INTERACTIVE` | multi-level feedback with sleep-credit boosting | humans |
| `SCHED_BATCH` | weighted fair share of what is left | everything else |

`SCHED_INFERENCE` is the interesting one. A token-generation loop never sleeps
waiting for a human, so an interactive heuristic gives it nothing; it has a
visible rate, so batch scheduling makes the machine feel broken. It is a soft
real-time stream with a deadline per token, and that is how it is scheduled.
When it overruns its budget it is demoted for the rest of its period rather
than allowed to starve the UI — a slow answer beats a frozen machine, and that
demotion is the line between the two.

Admission control is real: `sched_set_deadline` sums the utilisation of every
deadline thread and refuses work past 80% of the available cores. A real-time
class that accepts everything is indistinguishable from no real-time support.

---

## Capabilities

A capability is an object pointer, a rights mask, a generation number, an
unforgeable badge and a Kaalka seal. Four checks happen on every lookup, and
each corresponds to a specific failure mode:

- **type** — so a file handle cannot be passed where an endpoint is expected
- **rights** — so a read handle cannot write
- **generation** — so a revoked object cannot be reached through a stale slot
- **seal** — so a capability cannot outlive the authority that granted it

Revocation is a generation bump on the object. Every capability anywhere in the
system that points at it becomes stale at once, with no list to walk and no
chance of missing one.

Derivation is monotonic in both dimensions: `new_rights & parent_rights`, and a
lifetime clamped to the parent's remaining time. A derived capability can never
be stronger or longer-lived than what it came from.

---

## The runtime graph

Modelled on WebWeaveX, which captures a running system as a deterministic graph
with stable node identities. RESENTMENT applies it to the operating system
itself.

The canonical encoding of a node — and therefore the definition of its identity
— is: kind, label, every attribute in sorted key order with its type tag, then
the digest of every child in ascending digest order, then its outgoing edges.

What is **excluded** is the important part: timestamps, ids, versions and
pointers. Two machines that booted at different times but reached the same
configuration must hash identically, or cross-machine diffing is useless.

Cost control is what makes this survivable in a kernel:

- digests are computed lazily and cached; creating a node hashes nothing
- invalidation walks parent links only, so a deep change costs a walk to the
  root rather than a rehash of the tree
- the event ring is fixed-size with a lock-free producer, so recording from an
  interrupt handler is an atomic increment and a few stores
- high-rate event kinds (context switches, IRQs, capability checks) are off by
  default and can be enabled at runtime

The child fold uses a selection sort over the child list rather than an array,
because the digest function recurses and a stack array would multiply by tree
depth. That is marked in the source as a deliberate ceiling.

---

## Kaalka in the kernel

See [KAALKA.md](KAALKA.md) for the full treatment. The short version:

Kaalka gives the kernel a *time-keyed* key schedule. Epoch keys are derived
from a master secret, the epoch number, a purpose string and the clock angles
at the epoch boundary. All keys inside one epoch are identical; once the epoch
rolls they are unrecoverable, so a key recovered from a memory dump is stale
within a minute.

Seals bind a payload digest to a subject, a sequence number and a validity
window, with all of it inside the MAC. Verification checks the clock *before*
the MAC, so an expired seal is refused without a comparison that could leak
timing.

Confidentiality and integrity come from ChaCha20 and HMAC-SHA256, keyed by the
Kaalka derivation. The clock-angle stream is kept for interoperability with the
Kaalka libraries in other languages and is never used to protect kernel
objects. That separation is deliberate and stated in the header.

---

## The AI subsystem

Four things live here that normally live in a library, each because the kernel
is the only place they can be done correctly:

- **tensors** backed by `vm_objects`, so a tensor is shared between tasks with
  zero copies and revoked with a capability
- **a model registry** where weights are loaded once for the machine and
  content-addressed with a Kaalka-sealed digest, so a tampered model cannot be
  bound to an inference capability
- **a paged KV cache** where attention pages are content-addressed by the
  digest of the tokens they cover and shared between sequences with a common
  prefix; two sessions with the same system prompt store it once
- **an accelerator HAL** where CPU SIMD, a GPU queue and a phone NPU are the
  same object to a caller, with the CPU always present as the fallback

Floating point is illegal in kernel code except inside `rk_fpu_begin` /
`rk_fpu_end`, and only `kernel/ai/` is compiled with the vector unit enabled.
That is not conservatism: if the compiler may emit a vector instruction
anywhere, it will eventually emit one in an interrupt handler and silently
corrupt the interrupted thread's registers. The resulting bug is intermittent,
data-dependent and appears nowhere near its cause.

The advisor hook is the kernel's way of asking rather than deciding. It is
bounded three ways — a hard deadline, an answer clamped to the offered
candidate set, and a deterministic fallback — because an operating system may
consult a model but may never depend on one.

---

## SHE as the system language

SHE's defining idea is that a program starts with zero permissions. In
RESENTMENT that is not a language feature, it is the kernel security model, so
SHE is not hosted on the OS — it *is* the shell and policy language, and its
sandbox is the capability space.

The practical consequence: when a script tries to read a file without the
right, the kernel refuses at the capability check, the VM reports the exact
grant that was missing, and the same refusal would have happened to a compiled
binary. One mechanism, three kinds of caller.

Three deviations from the reference interpreter, all forced by ring 0:

- **integers and Q32.32 fixed point, not IEEE doubles** — the kernel avoids the
  FPU outside the AI subsystem, and the graph promises bit-identical replay
- **gas metering** — every script has an instruction budget, so a runaway loop
  in a boot script fails with a diagnostic instead of wedging the machine
- **bytecode, not a tree walker** — single-pass compilation bounds memory,
  which matters when the input arrived from a prompt inside the kernel

---

## Testing

The portable half of the kernel compiles for the host and runs against a
synthetic machine in `tests/host/`. It is the same source the kernel ships, not
a reimplementation: the shim supplies an arena, a clock and stubbed scheduling,
and the buddy allocator, slab heap, crypto, Kaalka, graph and SHE VM underneath
are real.

That harness found the alignment assumption in the slab allocator on its first
run, which is the argument for it.

On top of that:

- `tools/verify-image.py` checks the linked ELF the way a bootloader will:
  Multiboot2 header alignment and checksum, entry reachability, segment overlap
  and page alignment, higher-half placement
- `tools/kaalka_ref.py` implements the reference Kaalka in Python with floats
  and compares it byte for byte against the kernel's fixed-point version
- `rk_selftest_all()` runs on every boot, on the machine that is about to be
  trusted, because a kernel whose crypto is wrong should not be able to look
  healthy

---

## What is deliberately not here

Named so that the absences are choices rather than oversights:

- **No POSIX.** The syscall table is capability-oriented; most of what a POSIX
  kernel exposes is an IPC message to a service the caller holds an endpoint
  for. A compatibility layer would be a userspace server.
- **No network stack.** The device model and the driver binding are there; the
  protocol stack is not, and `rk_netdev_receive` counts and drops rather than
  pretending.
- **No ELF loader.** The ring-3 entry path and the SYSCALL MSRs are wired; the
  loader that would use them is not written.
- **One shared run queue, not per-CPU queues.** SMP works on all three
  architectures (see [SMP.md](SMP.md)), but balancing is by construction rather
  than by policy: any idle core takes the next runnable thread. Splitting the
  queue per core buys throughput on a large machine and costs a stealing policy,
  an imbalance metric and a class of subtle bug, so it waits for a profile that
  shows the lock actually contended.
- **No TLB shootdown traffic.** `RK_IPI_TLB` is wired end to end and handled;
  nothing sends it, because no userspace yet shares an address space across
  cores in a way that could unmap under one.
- **No NUMA awareness.** Every core is assumed equidistant from memory.
