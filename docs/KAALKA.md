# Kaalka in the kernel

[Kaalka](https://github.com/PIYUSH-MISHRA-00/Kaalka-Encryption-Algorithm)
derives keying material from the angles between the hands of a clock. In
userspace that is a cipher. In a kernel it is something more useful: a way to
make *time* a dimension of authority.

This document covers what the kernel does with it, what it deliberately does
not, and how the port is verified against the reference implementation.

---

## What the algorithm computes

At a given hour, minute and second, three hands have three pairwise
separations:

```
hour hand    30h + 0.5m + (0.5/60)s
minute hand  6m + 0.1s
second hand  6s

separation(a, b) = min(|a - b|, 360 - |a - b|)
```

Each separation selects a trigonometric function by the quadrant it falls in —
sine, cosine, tangent, cotangent for quadrants one to four — and the sum of the
three drives the byte transform. The hour is reduced modulo twelve, as a clock
face is.

You can watch this happen on a running machine:

```
resentment> angles()
{hour_minute: 143.883333, minute_second: 164.599999, hour_second: 20.716666}
```

The values change every second. That is the point.

---

## What the kernel uses it for

### 1. Temporal capability seals

Every capability carries a seal:

```c
struct kaalka_seal {
    u8             mac[32];
    kaalka_epoch_t epoch;
    s64            not_before;
    s64            not_after;
    u64            seq;
    u64            subject;
};
```

The MAC covers all of it — including the validity window — so widening the
window by editing the structure invalidates the seal. Verification checks the
clock **before** the MAC, so an expired seal is refused without a comparison
that could leak timing.

The consequence is the interesting part. In most capability systems, revocation
is a sweep somebody has to run, which means it is a sweep somebody forgets to
run. Here the default behaviour of a forgotten capability is to stop working.

Derivation clamps the child's lifetime to the parent's remaining time, so
authority cannot be laundered into something longer-lived than its source.

### 2. Epoch keying

An epoch is a coarse slice of wall time — sixty seconds by default. Keys are
derived per epoch, per purpose:

```c
void kaalka_derive_key(kaalka_epoch_t epoch, const char *purpose,
                       u8 *out, size_t outlen);
```

The derivation is HKDF-SHA256 over a master secret, with the epoch number, the
purpose string and **the clock angles at the epoch boundary** folded into the
info parameter. Folding the angles in is what makes the key genuinely
Kaalka-derived rather than merely timestamped: the same second on a different
day gives a different angle triple and therefore a different key.

Every key inside one epoch is identical, and once the epoch rolls they are
unrecoverable. A key lifted from a memory dump is stale within a minute.

Because the derivation is a pure function of `(master, epoch, purpose)`, it
reproduces exactly during deterministic replay.

### 3. Replay defence

An IPC envelope carries a sender, a receiver, a sequence number and a seal. A
bounded ledger of accepted `(subject, seq)` pairs rejects anything seen before:

```c
int kaalka_ledger_check(struct kaalka_ledger *l, u64 subject, u64 seq);
```

Bounded on purpose — an unbounded ledger is a memory exhaustion surface — and
self-expiring, because Kaalka already gives every entry a natural death: an
entry older than two epochs cannot be replayed anyway, since its seal has
expired.

---

## What it is deliberately not used for

**The clock-angle stream does not protect kernel objects.**

It is an additive cipher over a small key space. Anyone who knows roughly when
a message was sealed can search a day's worth of seconds — 43,200 possibilities
after the modulo-twelve reduction — in negligible time. Treating that as
confidentiality would be a mistake, and building a kernel on it would be a
serious one.

So the split is explicit:

| Layer | Provided by | Answering |
|---|---|---|
| *When* is this valid | Kaalka | epochs, seals, windows, replay |
| *Can it be forged* | HMAC-SHA256 | integrity |
| *Can it be read* | ChaCha20 | confidentiality |

`kaalka_envelope_seal` encrypts with ChaCha20 and authenticates with
HMAC-SHA256, both keyed by `kaalka_derive_key`. The envelope inherits Kaalka's
temporal properties without inheriting the weak cipher.

The classic transform is still implemented, and still tested, for exactly one
reason: interoperability. A payload produced by the Python, JavaScript, Java,
Kotlin or Dart Kaalka libraries decrypts here unchanged.

---

## The fixed-point port

The reference implementation uses IEEE doubles and libm. The kernel uses Q32.32
integer fixed point. Two reasons, both hard requirements rather than
preferences:

**The FPU is not available.** Kernel code outside `kernel/ai/` is compiled with
the vector unit disabled, because a compiler allowed to emit vector
instructions will eventually emit one in an interrupt handler and silently
corrupt the interrupted thread's registers. Seals are verified in exactly those
contexts.

**libm is not reproducible.** `sin()` on x86_64, ARM64 and RISC-V can differ in
the last bits. The runtime graph promises that a replayed run produces
identical digests, and a seal is an authorisation check — neither survives a
platform-dependent answer.

So the trig core is integer arithmetic with one rounding rule and a fixed
reduction order:

```c
typedef s64 kq_t;               /* Q32.32 */
#define KQ_SHIFT 32
kq_t kq_sin(kq_t degrees);      /* Taylor, terms derived from the previous one */
kq_t kq_tan(kq_t degrees);      /* clamped at +-2^20 */
```

Two documented departures:

- **Clamping.** The reference lets tangent run to infinity at 90 and 270
  degrees. A kernel cannot carry an infinity through an integer pipeline, so
  output is clamped to ±2²⁰. That changes a result only for angles within about
  10⁻⁶ degrees of an asymptote, and because encrypt and decrypt clamp
  identically, a round trip is still exact.
- **Rounding half away from zero**, matching how the reference rounds its
  offsets. JavaScript's `Math.round` rounds half toward positive infinity; the
  two differ only on an exact `.5`.

### Verification

`make kaalka-check` implements the reference in Python — float trig, the same
quadrant selection, the same offset formula — runs the kernel's implementation
through the host test binary, and compares byte for byte:

```
Kaalka: kernel fixed point vs reference floating point

  trig samples          360
  worst sin/cos error   3.336e-09   (Q32.32 resolution is 2.328e-10)

  stream transform      528/528 bytes identical  (100.00%)
  classic transform     528/528 bytes identical  (100.00%)

  The kernel's fixed-point implementation is byte-identical to the
  reference on every vector, so a payload produced by the Python, JS,
  Java, Kotlin or Dart Kaalka libraries decrypts here unchanged.
```

The host test suite additionally checks the algebraic identities (sin 30 = ½,
cos 0 = 1, the hands 90° apart at 3:00:00), determinism across repeated calls,
round trips at nine times of day, that one second of difference changes the
ciphertext, that widening a seal's window is detected, that an expired seal is
refused, and that a replayed envelope is rejected.

---

## Watching it work

```
resentment> .kaalka
epoch             29798263
epoch seconds     60
clock             05:43:39
angle hour-min    143883 millideg
angle min-sec     164599 millideg
angle hour-sec    20716 millideg
seals made        41
seals verified    38
seals rejected    0
seals expired     0
envelopes sealed  1
envelopes opened  1
replays blocked   1
rekeys            1
```

`replays blocked 1` is the boot self-test proving the ledger works on the
machine that is about to be trusted.
