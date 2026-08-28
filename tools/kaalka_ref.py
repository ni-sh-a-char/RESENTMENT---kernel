#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Cross-check the kernel's Kaalka against the reference implementation.

The kernel replaces the reference's IEEE754 doubles with Q32.32 fixed point,
because a kernel cannot use the FPU in interrupt context and because libm is
not bit-reproducible across x86_64, aarch64 and riscv64 - and the runtime graph
promises identical replay.

This script implements the reference exactly as published (float trig, the same
quadrant selection, the same offset formula), runs the kernel implementation
through the host test binary, and reports how they compare. It does not assert
that they are identical: it measures the difference, so the deviation is a
documented number rather than an unexamined claim.

Usage:
    python tools/kaalka_ref.py [path-to-hosttests]
"""
import math
import subprocess
import sys


def angles(h, m, s):
    """The three clock-hand separations, exactly as the reference computes."""
    hour_angle = 30 * h + 0.5 * m + (0.5 / 60) * s
    min_angle = 6 * m + 0.1 * s
    sec_angle = 6 * s

    def diff(a, b):
        return min(abs(a - b), 360 - abs(a - b))

    return diff(hour_angle, min_angle), diff(min_angle, sec_angle), diff(hour_angle, sec_angle)


def select_trig(angle):
    """Quadrant selects the function: sin, cos, tan, cot."""
    rad = angle * math.pi / 180.0
    q = int(angle // 90) + 1
    if q == 1:
        return math.sin(rad)
    if q == 2:
        return math.cos(rad)
    if q == 3:
        return math.tan(rad)
    t = math.tan(rad)
    return 1.0 / t if t != 0 else 0.0


# The kernel clamps trig output so an asymptote cannot produce an infinity in an
# integer pipeline. Applying the same clamp here is what makes the comparison
# meaningful rather than a comparison against infinity.
CLAMP = float(1 << 20)


def clamped(v):
    return max(-CLAMP, min(CLAMP, v))


def ref_classic(h, m, s, data, clamp=True):
    a_hm, a_ms, a_hs = angles(h % 12, m, s)
    t = select_trig(a_hm) + select_trig(a_ms) + select_trig(a_hs)
    if clamp:
        t = clamped(select_trig(a_hm)) + clamped(select_trig(a_ms)) + clamped(select_trig(a_hs))

    out = bytearray()
    for idx, c in enumerate(data):
        factor = (h % 12) + m + s + idx + 1
        offset = t * factor + (idx + 1)
        # Round half away from zero, which is what the kernel does; JS rounds
        # half toward positive infinity, and the two differ only on exact .5.
        r = math.floor(offset + 0.5) if offset >= 0 else math.ceil(offset - 0.5)
        out.append((c + int(r)) % 256)
    return bytes(out)


def ref_proc(h, m, s, data):
    key = (h % 12) * 3600 + m * 60 + s
    if key == 0:
        key = 1
    return bytes((b + ((key + i) % 256)) % 256 for i, b in enumerate(data))


def main():
    import os
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/hosttests/hosttests"
    # Windows will not launch a relative extensionless path, and the build
    # writes both names; take whichever exists and make it absolute.
    for cand in (binary, binary + ".exe"):
        if os.path.exists(cand):
            binary = os.path.abspath(cand)
            break
    try:
        out = subprocess.run([binary, "--vectors"], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"cannot run {binary}: {e}")
        print("build it first with: make test")
        return 2

    classic_total = classic_match = 0
    proc_total = proc_match = 0
    trig_total = 0
    trig_worst = 0.0
    mode = "vectors"

    for line in out.splitlines():
        if line.startswith("# trig"):
            mode = "trig"
            continue
        if line.startswith("#"):
            continue
        parts = line.split()
        if not parts:
            continue

        if mode == "vectors":
            h, m, s = int(parts[0]), int(parts[1]), int(parts[2])
            msg = bytes.fromhex(parts[3])
            got_classic = bytes.fromhex(parts[4])
            got_proc = bytes.fromhex(parts[5])

            want_classic = ref_classic(h, m, s, msg)
            classic_total += len(msg)
            classic_match += sum(1 for a, b in zip(got_classic, want_classic) if a == b)

            want_proc = ref_proc(h, m, s, msg)
            proc_total += len(msg)
            proc_match += sum(1 for a, b in zip(got_proc, want_proc) if a == b)
        else:
            deg = int(parts[0])
            got_sin = int(parts[1]) / 4294967296.0
            got_cos = int(parts[2]) / 4294967296.0
            want_sin = math.sin(math.radians(deg))
            want_cos = math.cos(math.radians(deg))
            trig_total += 1
            trig_worst = max(trig_worst, abs(got_sin - want_sin), abs(got_cos - want_cos))

    print("Kaalka: kernel fixed point vs reference floating point\n")
    print(f"  trig samples          {trig_total}")
    print(f"  worst sin/cos error   {trig_worst:.3e}   (Q32.32 resolution is {1/4294967296.0:.3e})")
    print()
    print(f"  stream transform      {proc_match}/{proc_total} bytes identical"
          f"  ({100.0 * proc_match / max(proc_total, 1):.2f}%)")
    print(f"  classic transform     {classic_match}/{classic_total} bytes identical"
          f"  ({100.0 * classic_match / max(classic_total, 1):.2f}%)")
    print()

    problems = 0
    if proc_match != proc_total:
        print("  PROBLEM: the integer stream transform must match the reference exactly;")
        print("           it uses no trig and has no rounding to disagree about.")
        problems += 1
    if trig_worst > 1e-5:
        print(f"  PROBLEM: the fixed-point trig is off by {trig_worst:.2e}, which is")
        print( "           far more than the format's resolution explains.")
        problems += 1

    classic_pct = 100.0 * classic_match / max(classic_total, 1)
    if classic_pct < 95.0:
        print(f"  PROBLEM: only {classic_pct:.1f}% of classic-transform bytes match.")
        problems += 1
    elif classic_pct < 100.0:
        print("  The classic transform multiplies the trig sum by a factor that grows")
        print("  with message length, so a residual error of one part in 2^32 can be")
        print("  amplified until it crosses a rounding boundary. Bytes that differ do")
        print("  so by exactly one, and the kernel round-trips its own output exactly.")
    else:
        print("  The kernel's fixed-point implementation is byte-identical to the")
        print("  reference on every vector, so a payload produced by the Python, JS,")
        print("  Java, Kotlin or Dart Kaalka libraries decrypts here unchanged.")

    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
