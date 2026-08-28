# Governance

Small project, honest structure. This document exists so that nobody has to
guess how a decision gets made.

## Who decides

RESENTMENT is maintained by **[@PIYUSH-MISHRA-00](https://github.com/PIYUSH-MISHRA-00)**,
who has final say on design and merges. That is a benevolent-dictator model, and
it is the right one at this size — pretending otherwise would be theatre.

It is intended to change. The path is written down below rather than left
implicit, because "how do I get commit access" should not be a question you have
to ask in private.

## Becoming a maintainer

There is no application. The maintainer invites people who have, over time:

- landed changes that needed real judgement, not just typing
- reviewed other people's changes usefully
- shown they will say "I don't know" and "this is wrong, including when I wrote
  it"

Maintainers get merge rights and are listed in `.github/CODEOWNERS`. Areas can
be owned separately — someone can own `arch/riscv64/` without owning
`kernel/cap/`.

## How decisions are made

**Ordinary changes.** One maintainer approval, CI green. If the author is a
maintainer, still one other approval — nobody merges their own non-trivial work.

**Design changes** — anything that alters an interface in `include/rk/`, the
capability model, the graph's canonical encoding, or the permission set SHE
enforces — go through an issue or a discussion first, with the alternatives
written down. The bar is not consensus; it is that the objections have been
answered rather than outlasted.

**Disagreement.** Argue it in the open thread. If it does not converge, the
maintainer decides and records the reasoning in the thread. A decision with a
written reason can be revisited later by someone who has new information; a
decision that was just fatigue cannot be.

**Reverting.** Anything can be reverted by any maintainer if it breaks the build
or the tests on any of the three architectures. Reverting is not a judgement of
the author; re-landing after a fix is the normal path.

## What "no" looks like

[ROADMAP.md](ROADMAP.md) has a section of things this project has decided not to
do, with the reason for each. If your proposal is in it, the answer is probably
no — but the reason is written down, so you can argue against the reason rather
than against the answer.

A closed issue is not a rebuke. It is an answered question.

## Releases

Semantic versioning on the kernel as a whole.

- **Major** — the capability model, the graph encoding, or the syscall ABI
  changes in a way that breaks something that worked.
- **Minor** — new subsystems, new architectures, new syscalls.
- **Patch** — fixes.

A release is tagged only when `make check` and `make qemu-test-all` both pass
from a clean tree. The release notes say what changed, and the version in
`CHANGELOG.md` is written before the tag, not after.

## Code of conduct

[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) applies to every space this project
uses. Enforcement is by the maintainer; report to
[@PIYUSH-MISHRA-00](https://github.com/PIYUSH-MISHRA-00) on GitHub, or through
a [private security advisory](https://github.com/ni-sh-a-char/RESENTMENT---kernel/security/advisories/new)
if the matter is sensitive.

## Licence and provenance

Apache 2.0. Contributions are accepted under the same licence, by the act of
opening the pull request — there is no CLA. If you contribute code you did not
write, say so and say where it came from; the 8x8 console font is in the tree
that way and is credited in `README.md`.
