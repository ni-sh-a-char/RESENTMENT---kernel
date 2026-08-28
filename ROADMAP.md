# Roadmap

What is done, what is next, and what this project deliberately will not do.

Dates are absent on purpose. This is a roadmap of order, not of schedule.

---

## Done — shipped in 2.0.0

- Boot to an interactive shell on **x86_64, aarch64 and riscv64**
- **SMP on all three**: ACPI MADT and a real-mode AP trampoline, PSCI `CPU_ON`,
  SBI HSM `hart_start`; tested on four and eight cores
- Buddy physical allocator, slab heap, address spaces, copy-on-write
- Four scheduling classes including `SCHED_INFERENCE`, deadline admission
  control, affinity, reschedule IPI
- Capabilities with no ambient authority, Kaalka-sealed, derivation that only
  ever weakens
- The runtime graph: Merkle digests over canonical encodings, snapshots,
  deterministic replay, the federated memory fabric
- Kaalka in Q32.32 fixed point, byte-identical to the reference implementation
- SHE: compiler, gas-metered VM, 29 builtins behind 11 permission grants, and
  `resh`
- VFS with ramfs, devfs, graphfs and an initrd
- 1440 host assertions, six QEMU targets, a boot image verifier, a Kaalka
  cross-check, and seven self-tests that run on every boot

---

## Next

### 1. Processes that can do more than compute and exit

**Ring 3 works on all three architectures** and is checked on every run. See
[docs/USERSPACE.md](docs/USERSPACE.md).

What is left is the interesting half:

- **The initial capability set** handed to a new process, and how a parent
  narrows it. This is the design question the whole kernel exists to answer and
  it should not be rushed to match POSIX.
- **`SYS_TASK_SPAWN`**, so a process can start another one rather than only the
  kernel and the shell being able to.
- **TLB shootdown.** `RK_IPI_TLB` is wired and handled but nothing sends it; it
  becomes necessary the moment two cores share an address space one of them
  unmaps.
- **`arch_pgtable_destroy` leaks** every level below the root. Fine while
  processes are rare, not once anything spawns in a loop.
- **aarch64 TTBR1.** Moving the kernel out of TTBR0 would free the bottom of
  every address space and let user programs be linked low, as they are on
  x86_64.

### 2. Storage and the network

- PCI and PCIe enumeration, MSI/MSI-X
- virtio-blk, virtio-net, virtio-console
- A block layer and a real on-disk filesystem
- A minimal IPv4/TCP stack, or a port of one. Capabilities make this more
  interesting than usual: a socket is an object with rights and an expiry, so
  "this process may talk to this host until Tuesday" is expressible.

### 3. The AI subsystem, past the first mile

**Done**: a model is read off a filesystem, parsed, and run through a real
transformer forward pass built from the kernel's own operators, on a thread the
deadline admission controller accepted. `.infer` does it on every architecture
and the suite checks it on every run.

What is left:

- **Put the decode loop on the paged KV cache.** The forward pass keeps its own
  contiguous history, because attention wants one head's keys consecutive and
  the paged cache stores sixteen-token blocks. The content addressing and
  prefix sharing are therefore not yet on the inference path, which is the
  single most interesting thing the cache does.
- **Quantised kernels** (Q4_K, Q8_0) with the SIMD paths the accelerator HAL
  already selects between. Everything today is f32.
- **A real model**, rather than the deterministic fixture. The fixture exists to
  prove the path, and says so; it makes no claim about output.
- **The advisor hook exercised for real**, against the deterministic fallback,
  with the difference measurable.

### 4. Hardening

- Per-CPU run queues, once contention on the shared one shows up in a profile
- TLB shootdown IPIs, needed as soon as a userspace can unmap under another core
- KASLR, stack canaries in the kernel, W^X audit of every mapping
- A fuzzing harness over the syscall surface and over the SHE compiler

---

## Wanted, unclaimed

Good entry points for a first contribution. None of these need the whole tree in
your head.

| Task | Where | Size |
|---|---|---|
| A `virtio-console` driver | `kernel/drivers/` | small |
| `ubsan` handlers so `-fsanitize=undefined` can be turned on | `kernel/core/` | small |
| More SHE builtins and their permission grants | `kernel/she/stdlib.c` | small |
| A `graphfs` node for the slab allocator's per-class statistics | `kernel/fs/graphfs.c` | small |
| Device tree parsing for the aarch64 GIC base addresses | `arch/aarch64/` | medium |
| A fourth architecture port | `arch/`, `docs/PORTING.md` | large |

---

## Explicitly not planned

Saying no is part of a roadmap.

- **POSIX compatibility.** RESENTMENT has no ambient authority. `open("/etc/passwd")`
  is not a thing a process can do here, and a compatibility layer that made it
  possible would delete the property the kernel exists for. A translation layer
  above the capability system is fine; a `fork`/`exec`/`uid` model underneath it
  is not.
- **A monolithic driver model.** Drivers hold capabilities like anything else.
- **Depending on a model to boot.** The advisor hook is bounded, clamped to the
  candidate set the kernel offered, and falls back to a deterministic heuristic
  when it is late or absent. An operating system may consult a model. It may
  never require one.
- **Floating point in the core kernel.** The FPU is unavailable in interrupt
  context and libm is not bit-reproducible across architectures. Only
  `kernel/ai/` is compiled with SIMD, behind `rk_fpu_begin`/`rk_fpu_end`.
