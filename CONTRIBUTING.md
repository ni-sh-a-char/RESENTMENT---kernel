# Contributing to RESENTMENT

Patches, bug reports and ports are welcome. This document is short because most
of what matters is enforced by `make check` rather than by review.

---

## Branches

| Branch | What it is |
|---|---|
| `main` | The current state of the project. Tagged releases are cut from here. |
| `develop` | Integration. Work lands here first when it is large enough to want a soak before it reaches `main`. |
| `release/2.x` | The 2.x line, at `v2.0.0`. Fixes for a shipped 2.x release go here and are merged forward. |
| `release/1.x` | The 1.x line, at `v1.0.0` - the original hobby kernel, frozen. Kept reachable because the starting point is part of the story. |

The release branches are named `release/N.x` rather than after the tags they
contain. A branch and a tag with the same name make every `git checkout` of
that name ambiguous, and git will refuse to create the second one; naming the
*line* rather than the *point* avoids the collision entirely and leaves the
tags to do what tags are for.

## Before you send anything

```sh
make check        # host tests, image verification, Kaalka cross-check
make all-arch     # all three architectures still build
make qemu-test    # the kernel still boots and its shell still answers
```

If you touched anything under `arch/`, say which machine you actually ran it on.
"Builds" and "boots" are different claims.

---

## What the code should look like

Match the surrounding file. Beyond that:

- **Tabs for indentation**, 8-wide, lines under 88 columns.
- **Kernel-style names**: `rk_` for portable API, `arch_` for the HAL, `x86_`
  and friends for architecture-private functions.
- **Errors are negative `RK_E*`**, success is `RK_OK`. No error codes invented
  ad hoc.
- **`__must_check` on anything whose failure would corrupt state** if ignored.
- **No floating point outside `kernel/ai/`.** The build enforces this; if you
  need it there, bracket it with `rk_fpu_begin()` and `rk_fpu_end()`.

---

## What the comments should say

The single rule: **comments explain why, not what.**

`i++; /* increment i */` is noise. What is worth writing down is the reason a
piece of code is shaped the way it is — the constraint that forced it, the
alternative that was rejected, the failure mode it prevents.

Good, from `kernel/mm/pmm.c`:

```c
/* Only usable memory decides how many struct pages we need. Firmware
 * routinely reports reserved and MMIO regions at very high addresses -
 * a PC has them just below 4 GiB and a server has them far above - and
 * sizing the array to cover those would cost gigabytes of metadata to
 * describe memory that can never be allocated. */
```

That comment exists because the kernel once tried to allocate 8 GiB of page
metadata on a 512 MiB machine.

If you make a deliberate simplification with a known ceiling, mark it and name
the upgrade path:

```c
/* ponytail: selection sort over the list, O(n^2) but O(1) extra memory.
 * This function recurses, so an array on the stack would multiply by the
 * tree depth; upgrade to an explicit worklist if a node ever legitimately
 * has thousands of children. */
```

`make` has no target that harvests these, but they are greppable and they are
how the tree stays honest about what it postponed.

---

## Tests

New portable logic needs a check in `tests/host/main.c`. The harness has no
framework — an assertion that prints what it expected and what it got is the
whole apparatus:

```c
CHECK(rk_strtou64("0xff", NULL, 0) == 255, "hex parse with prefix");
```

New kernel-only logic that can be checked cheaply belongs in
`kernel/core/selftest.c`, which runs on every boot. A kernel whose crypto is
wrong should not be able to look healthy.

New shell or language behaviour goes in the `CHECKS` list in
`tools/qemu-expect.py`.

**Write the test so that it fails if you break the thing.** The host suite once
reported fourteen passes against a kernel that had never received a byte of
input, because it searched the whole transcript instead of the reply.

---

## Adding an architecture

See [docs/PORTING.md](docs/PORTING.md). Implement `include/rk/arch.h` and a
boot stub; nothing above `arch/` may include an architecture header.

---

## Adding a driver

Fill in `struct rk_driver` with what it matches on — a PCI id, a device-tree
compatible string, an ACPI HID, or a platform name — and register it. The core
binds it to whatever the bus enumerated. Adding a driver changes no bus code,
and adding a bus changes no driver.

Drivers that do real work belong in a threaded handler
(`RK_IRQ_WAKE_THREAD`), not in the hard interrupt path. Work done there is
added to the worst-case latency of every real-time and inference task on the
machine, and this kernel makes promises about both.

---

## Security

If you find something exploitable, please open a private report rather than a
public issue.

Areas that deserve the most suspicion, because they take input from somewhere
that should not be trusted:

- `copy_from_user` / `copy_to_user` and every syscall handler that sizes a
  buffer from a user-supplied length
- the SHE compiler and VM — the input is a string from a prompt
- the GGUF, USTAR and device-tree parsers — all three read attacker-supplied
  lengths and offsets
- `cap_lookup` — it is the only thing standing between a task and every object
  in the kernel

---

## Licence

Contributions are under Apache 2.0, the same as the rest of the tree.
