# Changelog

All notable changes to this project are recorded here. This project follows
[semantic versioning](https://semver.org/).

## 2.0.0 — "kaalachakra"

The kernel becomes an operating system foundation. Version 1.0.0 printed one
line at boot. This one boots to an interactive shell in about 120 milliseconds
on **x86_64, aarch64 and riscv64**, uses every core the machine has, runs seven
self-tests on the machine it is about to be trusted on, and is verified by 1440
host assertions plus six QEMU targets.

### The three ideas

- **Capabilities that expire.** No ambient authority anywhere. Every capability
  carries a Kaalka seal with a validity window inside its MAC, so a forgotten
  permission stops working on its own. Derivation only ever weakens rights and
  shortens lifetime. Revocation is a generation bump, so every handle to an
  object goes stale at once with no list to walk.
- **The system as a Merkle DAG.** Every kernel object is a node with a SHA-256
  digest over a canonical encoding; the machine has one root hash. Timestamps,
  ids and pointers are excluded from that encoding so two machines in the same
  state hash identically. Snapshots are Kaalka-sealed. Events are recorded in
  causal order for deterministic replay.
- **Inference as a system resource.** `SCHED_INFERENCE` schedules token
  generation as the soft real-time stream it is. Attention caches are paged and
  content-addressed so sessions sharing a prefix store it once. Models carry a
  sealed digest and an unverified one cannot be bound to an inference
  capability.

### Subsystems

- **Memory** — buddy physical allocator with a DMA zone, slab heap with
  double-free detection, vm_objects with demand paging and copy-on-write,
  bounded user/kernel copies.
- **Scheduling** — four classes with EDF for the two deadline classes, real
  admission control, wait queues, sleeping locks, per-thread FPU state.
- **Kaalka** — deterministic Q32.32 trig, the reference byte transforms, epoch
  key derivation, seals, a bounded self-expiring replay ledger, and an
  authenticated envelope keyed by the epoch.
- **Crypto** — SHA-256, HMAC, HKDF, ChaCha20, a CSPRNG seeded from hardware
  entropy and interrupt jitter, SipHash, CRC32, hex and base64, all with
  known-answer tests from the published vectors.
- **Runtime graph** — lazy cached digests, canonical/JSON/text/graphviz export,
  a line-oriented diff, sealed snapshots, a causal event ring, and the
  federated memory fabric that survives reboots.
- **AI** — tensors on vm_objects, twenty operators including quantised formats,
  an accelerator HAL with a CPU fallback, a GGUF loader, paged attention with
  prefix sharing, an inference queue, and a bounded advisor hook.
- **SHE** — single-pass compiler to bytecode, a gas-metered VM, 29 builtins
  behind eleven permission grants, and `resh`, a shell with no grammar of its
  own.
- **Filesystems** — VFS with capability-scoped resolution, ramfs, devfs, the
  runtime graph as `/graph`, and a USTAR initrd mounted without copying.
- **IPC** — synchronous endpoints with single-use reply capabilities, buffered
  channels with credit flow control, notifications safe from interrupt context,
  and optional Kaalka sealing per endpoint.
- **Devices** — one model over platform, PCI, USB, virtio, I2C, SPI and MMIO
  buses; every device is a graph node.

### Architectures

All three boot to an interactive shell and pass the same test suite.

- **x86_64** — higher-half at −2 GiB with a 4 GiB direct map, Multiboot 1 and 2,
  APIC and I/O APIC with an 8259 fallback, TSC calibrated against the PIT,
  SYSCALL/SYSRET, IST stacks for the faults that cannot use the current stack,
  SMEP/SMAP/NX, kernel image hardened to r-x and r-- after boot, huge-page
  splitting so that hardening is exact.
- **aarch64** — EL2 to EL1 transition, a 39-bit-VA MMU with a 4 KiB granule,
  GICv2, generic timer, PL011, PSCI, full exception vectors, device tree
  parsing with a fault-tolerant RAM probe when the firmware provides no tree.
- **riscv64** — SBI timer, reset, IPI and hart state management; PLIC, NS16550,
  supervisor traps, and no assumption that the firmware chose hart zero.

### Symmetric multiprocessing

New in 2.0.0, and working on all three architectures. See
[docs/SMP.md](docs/SMP.md).

- **x86_64** — ACPI MADT discovery, with an RSDP scan for firmware that does not
  hand one over; a position-independent 16→32→64-bit application processor
  trampoline; the low identity map restored for the duration of start-up and
  removed again; per-core GDT, TSS and IST fault stacks; a shared IDT built
  once and loaded per core.
- **aarch64** — PSCI `CPU_ON`, with the boot record cleaned to the point of
  coherency because the starting core reads it with the MMU off; per-core GIC
  CPU interface and generic timer; logical core numbers in `TPIDR_EL1` rather
  than MPIDR, which is an affinity path and not an index.
- **riscv64** — SBI HSM `hart_start`; the first hart to arrive claims the boot
  role with an atomic swap, and the PLIC uses that hart's supervisor context.
- **The scheduler** — per-CPU state separated from a shared run queue, affinity
  honoured in every picker, a reschedule IPI so a woken thread does not wait for
  the next tick, and a thread returned to the run queue only after its stack
  pointer has actually been saved. That last one is the difference between an
  SMP kernel and a kernel that corrupts a stack under load.
- **Log output is serialised**, so two cores logging at once no longer interleave
  halfway through a line.

### Verification

- 1440 host assertions against the real kernel sources on a synthetic machine
- 7 self-tests on every boot, on the machine about to be trusted
- 26 shell checks driven over a real serial link under QEMU, against **six
  targets**: each of the three architectures single-core and again on four cores
- boot image structurally verified the way a bootloader will read it
- Kaalka cross-checked against the reference implementation: **100%
  byte-identical** on every vector

### Tooling

`make toolchain` fetches a portable cross-compiler for all three
architectures — no installer, no administrator rights. `make run` boots without
an ISO. `tools/addr2sym.py` turns a panic address into a name.
`tools/qemu-expect.py` is the integration harness. CI builds three
architectures, boots the kernel, drives its shell and produces an ISO.

### Fixed during bring-up

Each of these was found by the machine itself and each is now covered by a test:

- page metadata sized from reserved regions, asking for 8 GiB on a 512 MiB box
- the VGA console holding a physical pointer after the identity map was removed
- hardening a 2 MiB huge page making `.bss` read-only along with `.text`
- a new thread's stack misaligned by eight bytes, faulting on the first
  aligned vector instruction
- the run-queue lock held across a switch into a thread that never releases it
- the timer never started, so the idle thread halted and never woke
- a device registered before the device core's lock was initialised
- `/graph` files reporting size zero because they are generated on read
- an aarch64 alignment abort because with the MMU off every access is
  Device-nGnRnE, which forbids unaligned access outright — including the `stp`
  the compiler emitted while building the page tables that would turn it on
- ELR/SPSR and SEPC/SSTATUS not saved across a context switch taken inside a
  timer trap, on aarch64 and riscv64 respectively: the same latent bug, found
  once and fixed twice
- a reserve applied *after* a free block was published, shattering a 512 MiB
  run into order-0 pages
- OpenSBI booting hart 2 rather than hart 0, which the entry stub had assumed
- the Multiboot 1 repackaging step not being part of the default build, so an
  integration run could silently test yesterday's kernel

---

## 1.0.0

The original hobby kernel. A print message at boot time: x86_64, Multiboot 2,
VGA text output, a 1 GiB identity mapping, and an ISO you could boot in QEMU.

Kept as a tag because the starting point is part of the story.
