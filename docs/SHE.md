# SHE on RESENTMENT

SHE is the system language. The shell is a SHE interpreter, the boot script is
a SHE program, and an agent running on this kernel is a SHE program with a set
of grants. There is no separate shell grammar to learn.

This document is the reference for the kernel's implementation. The language
itself is [SHE](https://github.com/ni-sh-a-char/SHE); the differences forced by
running in ring 0 are listed at the end and each one is explained.

---

## The permission model

**A program starts with nothing.** Not "nothing dangerous" — nothing. It can
compute, and that is all. Every operation that reaches outside the program is
gated, and a refusal names the grant that was missing:

```
resentment> now()
not allowed to now.
  This script was not granted permission to read the clock.
  Run it with --allow-time to permit it.
```

That refusal is not the interpreter being cautious. The grant maps onto rights
in the kernel's capability space, and a compiled binary attempting the same
thing is refused in the same place by the same check.

| Grant | Flag | Allows |
|---|---|---|
| read | `--allow-read` | reading files, reading the memory fabric |
| write | `--allow-write` | writing files, writing the memory fabric |
| net | `--allow-net` | network access |
| run | `--allow-run` | starting programs |
| env | `--allow-env` | reading the environment |
| time | `--allow-time` | the clock, sleeping, Kaalka angles |
| random | `--allow-random` | the entropy pool |
| infer | `--allow-infer` | running models |
| graph | `--allow-graph` | reading the runtime graph, snapshots |
| device | `--allow-device` | touching devices |
| cap | `--allow-cap` | minting and granting capabilities |

Granting them:

```
resentment> .allow read           # at the prompt
resentment> .allowed              # what this session may do
```

```
qemu ... -append "run=/boot/bin/agent.she allow=read,graph once"
```

The shell starts with `time`, `graph` and `random`, because a prompt you cannot
ask the time from is a poor prompt and all three are read-only. The boot script
runs with everything, because it is the file the kernel was told to run — it is
trusted by position, the way `/sbin/init` is.

---

## Syntax

### Values and variables

```she
let name = "Piyush"        # text
let count = 42             # whole number
let ratio = 3.5            # real (fixed point, see below)
let ok = yes               # yes/no, also spelled true/false
let nothing_here = nothing

count = count + 1          # assignment needs no let
```

### Saying things

```she
say "hello"
say 2 + 2
say "hello, {name}!"       # interpolation reads a variable by name
```

### Conditions

`is` reads the way people say it. `is not` is its negation.

```she
if count is 42 then
  say "forty two"
else if count > 42
  say "more"
else
  say "less"
end
```

### Loops

```she
while count < 10
  count = count + 1
end

repeat
  count = count - 1
until count is 0

for each n in 1 to 5
  say n * n
end

for each item in [4, 8, 15]
  say item
end
```

`break` leaves a loop, `skip` goes to the next iteration.

### Functions

```she
fun double(x)
  return x * 2
end

say double(21)
```

Short functions are expressions, which is what makes them usable in pipelines:

```she
let is_even = fun(n) -> n % 2 is 0
```

### Pipelines

`x |> f(a)` calls `f(x, a)`. It reads left to right, in the order the work
happens:

```she
say [4, 8, 15, 16, 23, 42]
  |> filter(fun(n) -> n % 2 is 0)
  |> map(fun(n) -> n / 2)
  |> sum()
```

### Lists and maps

```she
let xs = [1, 2, 3]
say xs[0]                  # 1
say xs[-1]                 # 3, counting back from the end
push(xs, 4)
say xs.count               # 4

let m = {name: "resentment", cpus: 1}
say m["name"]
say m.cpus
```

### Comments

```she
# from a hash to the end of the line
```

---

## Builtins

Pure — usable with no permissions at all:

| | |
|---|---|
| `length(x)`, `count(x)` | items or characters |
| `upper(t)`, `lower(t)` | text case |
| `text(x)` | anything as text |
| `number(t)` | text as a number |
| `contains(a, b)` | does a hold b |
| `push(list, x)` | append |
| `sum(list)` | add up a list or range |
| `map(list, fn)` | apply to every item |
| `filter(list, fn)` | keep what a test accepts |
| `abs(n)`, `min(a,b)`, `max(a,b)` | arithmetic |

Guarded — each needs the grant shown:

| | Needs |
|---|---|
| `random(lo, hi)` | random |
| `now()`, `uptime()`, `sleep(ms)` | time |
| `angles()` | time — the live Kaalka clock-hand separations |
| `read(path)` | read |
| `write(path, text)` | write |
| `graph(format)` | graph — `"tree"`, `"json"`, `"canon"`, `"dot"` |
| `digest()` | graph — the whole machine as one hash |
| `snapshot()` | graph — a Kaalka-sealed state snapshot |
| `system()` | graph — a map of facts about this machine |
| `remember(key, value)` | write — store in the federated memory fabric |
| `recall(key)` | read — read it back, across reboots |
| `seal(value, seconds)` | cap — make a Kaalka seal |
| `models()` | infer — loaded models |

---

## The shell

Anything that is not a dot-command is SHE. A bare expression prints its value;
a statement does not.

| | |
|---|---|
| `.help` | the command list |
| `.allow <what>` / `.deny <what>` / `.allowed` | permissions |
| `.graph [tree\|json\|dot\|canon]` | the runtime graph |
| `.digest` | the Merkle root of the whole system |
| `.snapshot` | a sealed state snapshot |
| `.ps` `.mem` `.slab` `.sched` `.irq` `.caps` `.ipc` | subsystem state |
| `.kaalka` | temporal keying and the live clock angles |
| `.ai` | inference queue, KV cache, accelerators |
| `.events` `.stats` `.cpu` | the causal log and counters |
| `.cat <path>` `.ls [path]` `.run <path>` | files |
| `.dmesg` `.history` `.selftest` | diagnostics |
| `.reboot` `.poweroff` `.clear` | the machine |

---

## Differences from the reference interpreter

Three, all forced by running inside a kernel, and each one bought something.

**Numbers are integers and Q32.32 fixed point, not IEEE doubles.** The kernel
avoids the FPU outside the AI subsystem, because a vector instruction in an
interrupt handler silently corrupts the interrupted thread; and the runtime
graph promises bit-identical replay across x86_64, ARM64 and RISC-V, which
libm cannot deliver. Integer arithmetic stays exact — `7 / 2` produces the real
`3.500000` rather than truncating, and `6 / 2` stays the whole number `3`.

**Execution is gas-metered.** Every program has an instruction budget. A
runaway loop in a boot script stops with a diagnostic instead of wedging the
machine:

```
resentment> while true
              let x = 1
            end
(stopped: this used its whole instruction budget)
```

**Compilation is single-pass to bytecode, not to a syntax tree.** That bounds
memory, which matters when the input arrived from a prompt inside the kernel: a
tree-building parser can be made to allocate proportional to nesting depth by a
hostile input, and this cannot.

One limitation worth stating plainly: closures capture by **value** at the
moment they are created, not by reference. A lambda that mentions an enclosing
local gets a copy. That is what most scripts expect, and it removes an entire
class of lifetime bug from a language running in ring 0.
