# Security policy

## Reporting a vulnerability

Report privately, not in a public issue.

- **Preferred:** [open a draft security advisory](https://github.com/ni-sh-a-char/RESENTMENT---kernel/security/advisories/new)
  on this repository. That gives us a private thread and, if it matters, a CVE.
- **Alternative:** email `dev@gratefulworldventures.in` with `RESENTMENT` in the
  subject line.

Please include: the affected commit or tag, the architecture, what an attacker
gains, and the smallest reproduction you have — ideally a SHE script, a QEMU
command line, or a patch against `tests/host/`.

We will acknowledge within **five days** and give you an assessment within
**fourteen**. If we disagree that something is a vulnerability we will say so
and explain why, rather than letting the thread go quiet.

## Supported versions

| Version | Supported |
|---|---|
| `2.0.x` | yes |
| `1.0.x` | no — a pre-rewrite hobby kernel, kept as a tag for history |

## Scope

This is a kernel. Almost everything is security-relevant, but these are the
areas where a bug is a *vulnerability* rather than a defect:

| Area | Why |
|---|---|
| `kernel/cap/` | The capability check is the only thing between a task and the machine. A missing type, rights, generation or seal check is a privilege escalation. |
| `kernel/crypto/`, `kernel/crypto/kaalka.c` | Seal forgery, epoch key recovery, replay-window bypass. |
| `kernel/she/` | The gas meter and the permission gate on all 29 builtins. A builtin reachable without its grant is a sandbox escape. |
| `kernel/syscall/`, `arch/*/syscall*` | Argument validation at the user boundary: unchecked pointers, integer overflow in a length, TOCTTOU on a copied structure. |
| `kernel/mm/` | Anything that lets a task read or write memory it does not hold a capability for. |
| `kernel/ipc/` | Capability transfer across an endpoint. |

## What we already say out loud

These are documented design limits, not vulnerabilities. Reporting them is
welcome as a discussion, but they will not be treated as an embargoed issue.

- **The clock-angle stream is not the cipher.** Kaalka supplies the *schedule* —
  epoch keys, seal windows, replay defence. Confidentiality and integrity come
  from ChaCha20 and HMAC-SHA256. The clock-angle stream alone is an additive
  cipher over a small key space and is not used to protect kernel objects. See
  [docs/KAALKA.md](docs/KAALKA.md).
- **Temporal seals depend on the clock.** An attacker who controls the platform
  clock can widen a validity window. On a machine where that is in the threat
  model, seals need a monotonic source the attacker does not control.
- **Entropy at early boot is weak** and the kernel says so in its own log
  (`entropy pool is weak: 64 bits estimated`) until a hardware source or enough
  jitter has been mixed in.
- **No KASLR yet**, and no stack canaries in the kernel. Both are on the
  [roadmap](ROADMAP.md).
- **`build/` and `dist/` are not reproducible byte-for-byte** across toolchain
  versions. The Kaalka fixed-point port *is* bit-reproducible across
  architectures, which is a different and stronger claim, and `make kaalka-check`
  is what backs it.

## Disclosure

We prefer coordinated disclosure with a **90-day** default embargo, shortened if
a fix ships sooner and extended only by agreement. You will be credited in the
advisory and in `CHANGELOG.md` unless you ask not to be.

## This project's own security posture

- No dependencies. The kernel builds from its own sources plus a compiler.
- The build fetches a pinned `zig` and `nasm`; check
  [`tools/get-toolchain.sh`](tools/get-toolchain.sh) for the versions.
- Seven self-tests — allocator, crypto, Kaalka, graph, capabilities, AI, SHE —
  run on **every boot**, on the machine about to be trusted. A kernel whose
  crypto is wrong should not be able to look healthy.
