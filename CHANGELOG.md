# Changelog

## 0.2.0 — "kaalachakra"

The kernel becomes an operating system foundation. The previous version printed
one line at boot; this one boots to an interactive shell in about 100 ms, runs
seven self-tests on the machine, and builds for three architectures.

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

- **x86_64** — higher-half at −2 GiB with a 4 GiB direct map, Multiboot 1 and 2,
  APIC and I/O APIC with an 8259 fallback, TSC calibrated against the PIT,
  SYSCALL/SYSRET, IST stacks for the faults that cannot use the current stack,
  SMEP/SMAP/NX, kernel image hardened to r-x and r-- after boot, huge-page
  splitting so that hardening is exact.
- **aarch64** — EL2 to EL1 transition, GICv2, generic timer, PL011, PSCI, full
  exception vectors. Builds and links; identity-mapped, boot untested.
- **riscv64** — SBI timer and reset, PLIC, NS16550, trap vector. Builds and
  links; identity-mapped, boot untested.

### Verification

- 1440 host assertions against the real kernel sources on a synthetic machine
- 7 self-tests on every boot, on the machine about to be trusted
- 14 shell checks driven over a real serial link under QEMU
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

---

## 0.1.0

A kernel with a print message at boot time. x86_64, Multiboot2, VGA text
output, 1 GiB identity mapping.
