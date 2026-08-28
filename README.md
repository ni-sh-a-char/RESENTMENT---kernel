<p align="center">
  <img src="https://raw.githubusercontent.com/ni-sh-a-char/RESENTMENT---kernel/main/media/social-preview.png" alt="RESENTMENT 2.0.0 — a capability-secure, AI-native kernel" width="100%">
</p>

<p align="center">
  <a href="https://github.com/ni-sh-a-char/RESENTMENT---kernel/actions/workflows/ci.yml"><img src="https://github.com/ni-sh-a-char/RESENTMENT---kernel/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="LICENCE"><img src="https://img.shields.io/badge/licence-Apache--2.0-1c212b?style=flat-square&labelColor=08090c" alt="Apache 2.0"></a>
  <a href="docs/PORTING.md"><img src="https://img.shields.io/badge/arch-x86__64%20%7C%20aarch64%20%7C%20riscv64-4fd6d2?style=flat-square&labelColor=08090c" alt="x86_64, aarch64, riscv64"></a>
  <a href="#verification"><img src="https://img.shields.io/badge/tests-1440%20host%20%2B%20180%20QEMU-64d68a?style=flat-square&labelColor=08090c" alt="1440 host and 180 QEMU assertions"></a>
  <a href="https://buymeacoffee.com/piyushmishra00"><img src="https://img.shields.io/badge/buy%20me%20a%20coffee-e0a545?style=flat-square&logo=buymeacoffee&logoColor=08090c&labelColor=08090c" alt="Buy me a coffee"></a>
</p>

---

# RESENTMENT

**A capability-secure, AI-native kernel.**

```
  ####  RESENTMENT 2.0.0 "kaalachakra"
        a capability-secure, AI-native kernel
        x86_64 on pc, firmware multiboot2, 4 cpu
```

RESENTMENT is a from-scratch operating system kernel for x86_64, ARM64 and
RISC-V. It is not a Unix clone. It is built around three ideas that a
conventional kernel cannot express:

1. **Authority expires.** Every capability in the system carries a
   cryptographic seal with a time window. A forgotten permission stops working
   on its own, because expiry is the default rather than a revocation sweep
   somebody has to remember to run.

2. **The system is a graph you can hash.** Every kernel object is a node in a
   Merkle DAG. The whole machine has one SHA-256 root digest. Two systems in
   the same state produce the same hash, which makes system state something you
   can diff, attest and replay instead of something you infer from logs.

3. **Inference is a system resource.** Models, tensors and attention caches are
   kernel objects with their own scheduling class, because a userspace runtime
   cannot preempt a half-finished decode, cannot share weights between
   processes, and cannot stop a model reading memory it was never granted.

Three existing projects are load-bearing parts of the design, not
integrations bolted on the side:

| Project | What it is | What it does here |
|---|---|---|
| [**Kaalka**](https://github.com/PIYUSH-MISHRA-00/Kaalka-Encryption-Algorithm) | Time-driven encryption from clock-hand angles | The kernel's temporal authority layer: every capability seal, IPC envelope and snapshot is bound to a time window and an epoch key |
| [**WebWeaveX**](https://github.com/ni-sh-a-char/WebWeaveX) | Deterministic runtime graphs with stable node identity | The kernel's own state model: a Merkle DAG of every object, exportable, diffable, snapshottable and replayable |
| [**SHE**](https://github.com/ni-sh-a-char/SHE) | English-like language whose programs start with zero permissions | The system shell and policy language, with its sandbox enforced by the kernel's capability space rather than by an interpreter |

---

## Quick start

You need no cross-compiler. One command fetches a portable toolchain that
targets all three architectures:

```sh
make toolchain          # fetches zig + nasm into .toolchain/, no installer
make                    # build for x86_64
make test               # run the host test suite
make iso && make run    # boot it under QEMU
```

Or use the Docker image, which needs nothing on your machine:

```sh
docker build -t resentment-build buildenv
docker run --rm -v "$PWD":/root/env resentment-build make iso
```

Build for the other architectures:

```sh
make ARCH=aarch64
make ARCH=riscv64
make all-arch           # all three; this is the portability check
```

---

## What it looks like

The shell is a SHE interpreter, so there is no separate shell grammar. A
command line is a program:

```
resentment> 2 + 2
4

resentment> [4, 8, 15, 16, 23, 42] |> filter(fun(n) -> n % 2 is 0) |> sum()
104

resentment> for each n in 1 to 3
              say "n is {n}"
            end
n is 1
n is 2
n is 3
```

Scripts start with **no permissions**. When one tries to reach outside itself,
the kernel refuses and the message names the grant that was missing:

```
resentment> now()
not allowed to now.
  This script was not granted permission to read the clock.
  Run it with --allow-time to permit it.

resentment> .allow time
granted permission to read the clock
resentment> now()
1735689600
```

The whole machine is readable as a filesystem:

```
resentment> .ls /graph
  graph  digest        the SHA-256 merkle root of the entire system
  graph  tree          every kernel object as an indented tree
  graph  json          the same, deterministically
  graph  events        the causal event log
  graph  threads       every thread and its scheduling class
  graph  kaalka        temporal keying state and the live clock angles
  graph  ai            inference queue, KV cache, accelerators
  ...

resentment> .digest
7d4a1f0e83c25b9a6f1e0d4c8b3a7e2f5d9c1b8a4e7f0c3d6a9b2e5f8c1d4a7b
```

---

## Design

### Capabilities that expire

There is no ambient authority. A task cannot open a file because it knows a
path; it can only act on objects it holds a capability for. Each capability is
checked four ways on every use — object type, rights, generation, and a Kaalka
seal — and each check exists because of a specific way capability systems fail:

```c
int cap_lookup(struct capspace *cs, cap_handle_t h, enum cap_type type,
               u32 need_rights, struct cap_object **out);
```

The seal is the part a conventional design does not have. Revocation elsewhere
is a sweep. Here every capability has a deadline inside its MAC, so the default
behaviour of a forgotten capability is to stop working, and widening the window
by editing the struct invalidates the seal.

Derivation only ever weakens: rights are intersected with the parent's and the
lifetime is clamped to the parent's remaining time, so the authority reachable
from a capability is bounded by the capability itself.

### The runtime graph

Every object the kernel creates is a node carrying a SHA-256 digest over a
canonical encoding of its own fields plus the sorted digests of its children.
Deliberately excluded from that encoding: timestamps, ids, pointers and
versions — so two machines that booted at different times but reached the same
configuration hash identically.

That one property gives four things:

- **introspection** — an agent reads typed state, not scraped text
- **diffing** — identical subtrees are one identical line and are skipped
- **attestation** — the root digest is Kaalka-sealed, so a snapshot proves what
  the system looked like *and when*
- **replay** — events are recorded in causal order with a virtual clock, so a
  crash becomes reproducible rather than anecdotal

Digests are lazy and cached; a change invalidates only its ancestors. Recording
is a fixed ring with per-kind enables, so tracing costs an atomic increment and
does not have to be turned off.

### Inference as a scheduling class

A token-generation loop is not interactive — it never sleeps for a human, so
sleep-credit heuristics give it nothing. It is not batch either — it has a rate
the user can see. It is a soft real-time stream, and `SCHED_INFERENCE` treats
it as one: admitted with a declared rate, given a per-period budget, and
demoted rather than dropped when it overruns.

Attention caches are paged like memory, because they behave like memory:
allocated in blocks, mostly cold, and identical between sequences that share a
prefix. Pages are content-addressed by the digest of the tokens they cover, so
two sessions with the same system prompt store it once.

The kernel can also *ask*: a bounded advisor hook lets a model answer policy
questions that have no correct static answer — which page to evict, where to
place a thread. Advisors run with a hard deadline, their answers are clamped to
the candidate set the kernel offered, and a late or missing advisor falls back
to the deterministic heuristic. An operating system may consult a model; it may
never depend on one.

### Kaalka in a kernel

Kaalka derives keying material from the angles between the hands of a clock. In
userspace that is a cipher. Here it is how *time* becomes a dimension of
authority: epoch keys that are unrecoverable seconds later, capability seals
that expire by construction, and replay defence with no protocol of its own.

Two deliberate departures from the reference implementation, both documented
and both verified:

- **Fixed point, not floating point.** A kernel cannot use the FPU in interrupt
  context, and libm is not bit-reproducible across x86_64, ARM64 and RISC-V.
  The trig core is Q32.32 with one rounding rule. `make kaalka-check` compares
  it against the reference and currently reports **100% byte-identical output
  on every vector**, with a worst-case sine error of 3.3e-9.
- **Kaalka supplies the schedule; ChaCha20 and HMAC-SHA256 supply the strength.**
  The clock-angle stream alone is an additive cipher over a small key space and
  is not used to protect kernel objects. Saying so plainly is part of the design.

---

## Status

Honest, because a status table that overstates is worse than none.

| Subsystem | State |
|---|---|
| Boot to an interactive shell | **working on all three architectures**, ~120 ms |
| Wall clock | **working on all three** — CMOS, PL031, goldfish; Kaalka keys from real time everywhere |
| Initial ramdisk | **working on all three** — a bootloader module when there is one, otherwise linked into the image |
| x86_64: higher-half, Multiboot 1 and 2, APIC, huge pages | **working**, tested |
| aarch64: EL2→EL1, MMU, GICv2, generic timer, PL011, PSCI | **working**, tested |
| riscv64: SBI, PLIC, NS16550, supervisor traps | **working**, tested |
| SMP | **working on all three** — ACPI MADT + AP trampoline, PSCI `CPU_ON`, SBI HSM; tested on 4 and 8 cores |
| Physical memory (buddy), heap (slab) | **working**, tested |
| Paging, address spaces, copy-on-write | **working on all three** — 4-level on x86_64, 39-bit on aarch64, Sv39 on riscv64 |
| Scheduler, 4 classes, deadline admission, affinity | **working**, multiprocessor, reschedule IPI |
| Capabilities, Kaalka seals, revocation | **working**, tested |
| Runtime graph, digests, snapshots, memfab | **working**, tested |
| SHE compiler, VM, stdlib, shell | **working**, tested |
| Kaalka: trig, transforms, seals, envelopes | **working**, cross-checked byte-for-byte |
| Crypto: SHA-256, HMAC, HKDF, ChaCha20, CSPRNG | **working**, known-answer tested |
| VFS, ramfs, devfs, graphfs, initrd | **working** |
| IPC endpoints, channels, notifications | **working** |
| AI: tensors, ops, accel HAL, KV cache, model registry | **working**, GGUF parsed |
| Inference end to end | **working on all three** — a real transformer forward pass through the kernel's own operators, on a thread admitted by deadline admission control |
| Console: VGA, framebuffer, serial, PS/2 | **working** |
| Syscall entry (SYSCALL/SYSRET, int 0x80) | **working** |
| Ring-3 userspace | **working on all three** — ELF64 loader, per-process address spaces, syscalls from user mode |
| PCI, block, network drivers | device model only — see [ROADMAP](https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/ROADMAP.md) |

### Verification

| Command | What it proves |
|---|---|
| `make test` | 1440 assertions against the real kernel sources on a synthetic machine |
| `make verify` | the linked image is loadable, checked the way a bootloader reads it |
| `make kaalka-check` | the fixed-point port is byte-identical to the reference |
| `make qemu-test` | the kernel boots and its shell answers correctly over a serial link |
| `make qemu-test-all` | all three architectures, each on one core and on four |
| `make check` | the first three together |

`make qemu-test-all` is the one that matters before a release. It builds every
architecture, boots each twice — once single-core, once with `-smp 4` — and
drives 30 assertions through the shell over a serial socket each time - The last few run the attestation demo and load a program into ring 3, which is the scenario the whole design
exists for: seal a snapshot, read the clock angles Kaalka is keying from, prove
the digest holds across pure computation, then change the machine and watch its
name change with it.

```
=== x86_64 ==================================================
  x86_64: all 30 checks passed
=== aarch64 =================================================
  aarch64: all 30 checks passed
=== riscv64 =================================================
  riscv64: all 30 checks passed
=== x86_64-smp ==============================================
  x86_64-smp: all 30 checks passed
=== aarch64-smp =============================================
  aarch64-smp: all 30 checks passed
=== riscv64-smp =============================================
  riscv64-smp: all 30 checks passed

every check passed on x86_64, aarch64, riscv64, x86_64-smp, aarch64-smp, riscv64-smp
```

Plus seven self-tests that run on every boot, on the machine about to be
trusted — allocator, crypto, Kaalka, graph, capabilities, AI, SHE. A kernel
whose crypto is wrong should not be able to look healthy.

---

## Layout

```
include/rk/      the entire kernel API, one header per subsystem
arch/            x86_64, aarch64, riscv64 — the only machine-specific code
  */linker.ld    where the image lands
kernel/
  core/          boot, log, panic, irq, time, device model, self-tests
  lib/           freestanding libc subset, printf, fdt, 128-bit division
  mm/            buddy allocator, slab heap, address spaces
  sched/         four scheduling classes, wait queues
  sync/          mutex, semaphore, rwlock, condvar, completion
  cap/           capabilities and capability spaces
  ipc/           endpoints, channels, notifications
  fs/            VFS, ramfs, devfs, graphfs, initrd
  crypto/        SHA-256, HMAC, HKDF, ChaCha20, CSPRNG, Kaalka
  graph/         the runtime graph and the federated memory fabric
  ai/            tensors, operators, accelerators, models, KV cache, scheduler
  she/           the SHE compiler, VM, standard library and shell
  syscall/       the system call table
user/            SHE programs that become the initial ramdisk
tests/host/      the portable kernel compiled for the host, with assertions
tools/           build, test, image verification, Kaalka cross-check
docs/            architecture, porting, and per-subsystem design notes
```

Porting to a new machine means implementing `include/rk/arch.h` and a boot
stub. Nothing above `arch/` includes an architecture header.

---

## Documentation

- [Architecture](docs/ARCHITECTURE.md) — how the pieces fit and why
- [Building](docs/BUILDING.md) — toolchains, targets, running under QEMU
- [The SHE language](docs/SHE.md) — syntax, builtins, the permission model
- [Kaalka in the kernel](docs/KAALKA.md) — temporal authority, seals, epochs
- [The runtime graph](docs/GRAPH.md) — digests, snapshots, replay, the fabric
- [The AI subsystem](docs/AI.md) — tensors, scheduling, paged attention
- [Porting](docs/PORTING.md) — what a new architecture has to provide
- [SMP](docs/SMP.md) — how each architecture starts its other cores
- [Ring 3](docs/USERSPACE.md) — the ELF loader, and what a process may do
- [The AI subsystem](docs/AI.md) — tensors, scheduling, and the forward pass
- [Contributing](https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/CONTRIBUTING.md) · [Roadmap](https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/ROADMAP.md) · [Security](https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/SECURITY.md) · [Governance](https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/GOVERNANCE.md)

The full documentation site, including everything above rendered for reading
rather than for grepping, is at **<https://ni-sh-a-char.github.io/RESENTMENT---kernel/>**.

---

## History

RESENTMENT began as a hobby kernel that printed one line at boot. That line is
still here, in `kernel/drivers/console/vga.c`, now one console backend among
several rather than the only thing the kernel could do.

`v1.0.0` tags that kernel as it stood: a Multiboot stub that reached long mode
and wrote to the VGA text buffer. `v2.0.0` is this tree. Both tags stay
reachable, because the starting point is part of the story.

| Branch | What it holds |
|---|---|
| `main` | everything: the kernel, the documentation site, and the community files. Releases are tagged from here. |
| `v2.0.0` | the kernel alone — sources, headers, tests, build and technical docs. No website, no community files. |
| `v1.0.0` | the original hobby kernel, frozen at the `v1.0.0` tag |
| `develop` | integration, for work that wants a soak before it reaches `main` |

`README.md` is identical on `main` and `v2.0.0`, which is why the links above
that point at community files are absolute: those files live only on `main`.
`make site` and `make media` likewise only work there.

A branch and a tag here share a name, which git allows but treats as ambiguous:
`git checkout v2.0.0` will warn. Say `git checkout refs/heads/v2.0.0` for the
branch or `git checkout refs/tags/v2.0.0` for the tag when it matters.

## Press kit

`media/` carries the social preview cards and a document with ready-to-post
copy, the asset guide and the talking points — including an honest list of what
is *not* done, which is the section that matters most when a technical audience
reads it.

| File | What it is |
|---|---|
| `media/social-preview.png` | 1280×640 — the GitHub repository social preview |
| `media/social-wide.png` | 1600×900 — X and anything 16:9, with a real boot transcript |
| `media/social-linkedin.png` | 1200×627 — LinkedIn and Facebook link cards |
| `media/RESENTMENT-2.0.0-social-kit.docx` | posts, talking points, fact sheet |

All of it lives on `main` and regenerates with `make media`. The cards come
from [`tools/mksocial.py`](https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/tools/mksocial.py) and the document from
[`tools/mkdocx.py`](https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/tools/mkdocx.py), so the numbers in them cannot drift from
the numbers in the suite.

---

## Support

RESENTMENT is built in the open and will stay that way. If it is useful to you,
or you simply enjoyed reading how the bugs were found, you can
[buy me a coffee](https://buymeacoffee.com/piyushmishra00).

<a href="https://buymeacoffee.com/piyushmishra00"><img src="https://img.shields.io/badge/buy%20me%20a%20coffee-e0a545?style=for-the-badge&logo=buymeacoffee&logoColor=08090c&labelColor=08090c" alt="Buy me a coffee"></a>

## Licence

Apache 2.0. See [LICENCE](LICENCE).

The 8x8 console font is public domain, by Daniel Hepper, derived from the IBM
VGA fonts.
