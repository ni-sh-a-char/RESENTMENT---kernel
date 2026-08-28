<!--
Thanks for sending this. A short description of *why* is worth more than a long
description of *what* — the diff already says what.
-->

## What this changes

## Why

<!--
What was wrong, or what could not be expressed before. If it fixes an issue,
"Fixes #N" here.
-->

## How it was tested

<!-- Delete the lines that do not apply, and paste the output that matters. -->

- [ ] `make check` — host tests, image verification, Kaalka cross-check
- [ ] `make qemu-test-all` — every architecture, single core and on four
- [ ] Booted on real hardware (say which)
- [ ] New assertions in `tests/host/` covering the change
- [ ] Not testable automatically, because:

```
paste the output here
```

## Checklist

- [ ] It builds for **all three** architectures (`make all-arch`)
- [ ] No new dependency. This kernel builds from its own sources plus a compiler.
- [ ] Comments explain *why*, not *what*. See [CONTRIBUTING.md](../CONTRIBUTING.md).
- [ ] A deliberate simplification with a known ceiling is marked with a
      `ponytail:` comment naming the ceiling and the upgrade path
- [ ] `docs/` updated if this changes an interface or a design decision
- [ ] `CHANGELOG.md` updated if a user would notice

## Anything you are unsure about

<!--
Genuinely useful. "I could not decide between X and Y" gets a better review
than silence.
-->
