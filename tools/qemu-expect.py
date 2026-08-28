#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Boot RESENTMENT under QEMU, drive its shell, and check what comes back.

A kernel that compiles is not a kernel that works, and a kernel that boots is
not a kernel whose shell works. This connects to the guest's serial line over
TCP - which behaves the same on every host, unlike redirected stdin - types
commands at the prompt and matches the replies.

It is the integration test: the boot path, the interrupt controller, the timer,
the UART, the scheduler, the SHE compiler and VM, and the capability sandbox
all have to work for a single line to come back correct.

Usage:
    python tools/qemu-expect.py                       # x86_64
    python tools/qemu-expect.py --arch aarch64
    python tools/qemu-expect.py --arch riscv64
    python tools/qemu-expect.py --all                 # every architecture
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

BASE_PORT = 45123
ANSI = re.compile(rb"\x1b\[[0-9;]*m")

# How each architecture is booted, and which kernel file to hand QEMU.
#
# x86_64 uses the repackaged 32-bit ELF because QEMU's Multiboot loader refuses
# a 64-bit one; the other two take the ELF directly. Only x86 gets an initrd,
# because QEMU passes modules to a Multiboot kernel and has no equivalent for a
# bare ELF on the virt machines.
TARGETS = {
    "x86_64": {
        "qemu":   "qemu-system-x86_64",
        "kernel": "dist/x86_64/resentment32.elf",
        "args":   ["-m", "512M", "-no-reboot"],
        "initrd": "dist/x86_64/initrd.tar",
    },
    "aarch64": {
        "qemu":   "qemu-system-aarch64",
        "kernel": "dist/aarch64/resentment.elf",
        "args":   ["-M", "virt", "-cpu", "cortex-a72", "-m", "512M", "-no-reboot"],
        "initrd": None,
    },
    "riscv64": {
        "qemu":   "qemu-system-riscv64",
        "kernel": "dist/riscv64/resentment.elf",
        "args":   ["-M", "virt", "-m", "512M", "-no-reboot"],
        "initrd": None,
    },
}

# The same images on four cores. Everything the single-core run checks has to
# still hold, plus the cores actually have to come up: a kernel that boots on
# one core and wedges on four is a kernel that works on nobody's laptop.
# The one check that cannot be shared: loading an ELF, dropping to ring 3 and
# coming back through a system call. Only x86_64 has userspace so far.
USERSPACE_CHECKS = [
    (".exec /boot/bin/init", "hello from ring 3", True),
]
for _arch in ("x86_64", "aarch64", "riscv64"):
    TARGETS[_arch]["checks"] = list(USERSPACE_CHECKS)

# Running a transformer, on the other hand, is pure arithmetic and works
# everywhere. This is the check behind the phrase "AI-native": a GGUF model
# read off a filesystem, a real forward pass through the kernel's own
# operators, and the work admitted by deadline admission control.
INFERENCE_CHECK = (".infer 8", "SCHED_INFERENCE", False)
for _t in TARGETS.values():
    _t.setdefault("checks", [])
    _t["checks"] = list(_t["checks"]) + [INFERENCE_CHECK]

for _arch, _extra in (("x86_64", ["ACPI reports 4 usable processors"]),
                      ("aarch64", []),
                      ("riscv64", [])):
    TARGETS[_arch + "-smp"] = dict(
        TARGETS[_arch],
        args=TARGETS[_arch]["args"] + ["-smp", "4"],
        must=_extra + ["4 of 4 processors started",
                       "cpu1 online", "cpu2 online", "cpu3 online"],
    )


class Session:
    def __init__(self, sock, log):
        self.sock = sock
        self.buf = b""
        self.log = log

    def read_prompt(self, timeout=20.0, start=0):
        """Wait for either prompt.

        A block typed a line at a time gets the continuation prompt instead of
        the main one, and a harness that only knows about the main one hangs on
        the first multi-line function.
        """
        deadline = time.time() + timeout
        while True:
            seen = ANSI.sub(b"", self.buf[start:])
            if b"resentment>" in seen or b"... " in seen:
                return True
            if time.time() > deadline:
                return False
            self.sock.settimeout(0.4)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return False
            if not chunk:
                return False
            self.buf += chunk
            self.log.write(chunk)

    def read_until(self, needle, timeout=25.0, start=0):
        """Wait for text to appear after `start`.

        Searching from the beginning would match the prompt that was already
        there, so every command would appear to complete instantly and every
        check would run against an empty reply.
        """
        deadline = time.time() + timeout
        needle = needle.encode() if isinstance(needle, str) else needle
        while needle not in ANSI.sub(b"", self.buf[start:]):
            if time.time() > deadline:
                return False
            self.sock.settimeout(0.4)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return False
            if not chunk:
                return False
            self.buf += chunk
            self.log.write(chunk)
        return True

    def send(self, line):
        self.sock.sendall(line.encode() + b"\r")

    def text(self):
        return ANSI.sub(b"", self.buf).decode("utf-8", "replace")

    def mark(self):
        """Remember where the transcript is now, so a reply can be isolated.

        Searching the whole buffer for the expected text is what makes an
        expect script lie: the boot log contains almost every short string
        somewhere, so a check for "4" passes before anything is even typed.
        """
        return len(self.buf)

    def since(self, mark):
        return ANSI.sub(b"", self.buf[mark:]).decode("utf-8", "replace")


# (command, expected substring or None, needs_initrd)
CHECKS = [
    ("2 + 2",                           "4",     False),
    ("17 % 5",                          "2",     False),
    ("7 / 2",                           "3.5",   False),
    ('say "hello from the shell"',      "hello from the shell", False),
    ("let xs = [4, 8, 15, 16, 23, 42]", None,    False),
    ("sum(xs)",                         "108",   False),
    # evens are 4, 8, 16 and 42; halved they are 2, 4, 8 and 21; sum 35.
    ("xs |> filter(fun(n) -> n % 2 is 0) |> map(fun(n) -> n / 2) |> sum()",
                                        "35",    False),
    ("length(xs)",                      "6",     False),
    ('let who = "world"',               None,    False),
    ('say "hello, {who}!"',             "hello, world!", False),
    ("fun fact(n)",                     None,    False),
    ("  if n <= 1 then",                None,    False),
    ("    return 1",                    None,    False),
    ("  end",                           None,    False),
    ("  return n * fact(n - 1)",        None,    False),
    ("end",                             None,    False),
    ("fact(10)",                        "3628800", False),
    # The sandbox: an ungranted permission is refused, and the refusal names
    # the flag that grants it.
    ('read("/boot/etc/boot.she")',      "--allow-read", True),
    (".allow read",                     "granted",      True),
    ('length(read("/boot/etc/boot.she")) > 0', "yes",   True),
    # The machine's own state, addressable.
    (".digest",                         None,    False),
    (".ls /graph",                      "kaalka", False),
    ("length(digest()) is 64",          "yes",   False),
    # The graph digest must move when kernel state changes and not otherwise.
    ("let d1 = digest()",               None,    False),
    ("let junk = [1,2,3] |> map(fun(n) -> n * 2)", None, False),
    ("digest() is d1",                  "yes",   False),
    # The real-life scenario the whole design is for: take a sealed snapshot of
    # the machine, read the clock angles Kaalka is keying from, prove the digest
    # is stable across pure computation, then change the machine and watch its
    # name change with it. Last, because it grants every permission.
    (".allow all",                      "granted", False),
    (".run /boot/bin/attest.she",       "comparing two hashes", False),
]

BOOT_MUST_CONTAIN = [
    "self-tests passed",
    "boot complete",
    "system state digest",
    "kaalka self-test passed",
    "crypto self-test passed",
]


def run_one(arch, qemu_override, timeout, port):
    spec = TARGETS[arch]
    qemu = qemu_override or spec["qemu"]
    kernel = spec["kernel"]

    print(f"\n=== {arch} " + "=" * (56 - len(arch)))
    if not os.path.exists(kernel):
        print(f"  {kernel} does not exist; run make ARCH={arch} first")
        return 1

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1)

    cmd = [qemu, "-kernel", kernel, "-display", "none"] + spec["args"]
    cmd += ["-serial", f"tcp:127.0.0.1:{port}"]

    has_initrd = spec["initrd"] and os.path.exists(spec["initrd"])
    if has_initrd:
        cmd += ["-initrd", spec["initrd"]]

    qemu_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.STDOUT)

    listener.settimeout(25)
    try:
        sock, _ = listener.accept()
    except socket.timeout:
        qemu_proc.kill()
        print("  QEMU never connected to the serial port")
        return 1

    failures = 0
    os.makedirs("build", exist_ok=True)
    with open(f"build/qemu-expect-{arch}.log", "wb") as log:
        s = Session(sock, log)

        if not s.read_until("resentment>", timeout):
            print("  never reached the shell prompt. Last output:\n")
            print(s.text()[-1500:])
            qemu_proc.kill()
            return 1
        print("  reached the shell prompt")

        boot = s.text()
        for want in BOOT_MUST_CONTAIN + spec.get("must", []):
            if want not in boot:
                print(f"  MISSING from the boot log: {want}")
                failures += 1

        # Every image carries its own copy of the ramdisk, so /boot exists on
        # every architecture whether or not the loader passed a module. A check
        # that needs it is therefore never skipped - if /boot is missing, that
        # is a failure and should read as one.
        # Ring 3 exists on x86_64 only for now, so the check that proves it is
        # attached to those targets rather than asserted everywhere.
        checks = CHECKS + spec.get("checks", [])

        ran = 0
        for command, expect, _needs_initrd in checks:
            ran += 1
            mark = s.mark()
            s.send(command)
            if not s.read_prompt(20.0, start=mark):
                print(f"  TIMEOUT on: {command}")
                failures += 1
                continue
            reply = s.since(mark)
            if expect and expect not in reply:
                print(f"  FAIL  {command}")
                print(f"        expected {expect!r}, got:")
                for line in reply.splitlines():
                    if line.strip():
                        print(f"          {line}")
                failures += 1
            elif expect:
                print(f"  ok    {command}  ->  {expect}")

        s.send(".poweroff")
        time.sleep(1.0)

    try:
        qemu_proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        qemu_proc.kill()
    listener.close()

    if failures:
        print(f"  {arch}: {failures} check(s) failed")
    else:
        print(f"  {arch}: all {ran} checks passed")
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="x86_64", choices=list(TARGETS))
    ap.add_argument("--all", action="store_true", help="test every architecture")
    ap.add_argument("--qemu", default=None, help="path to the QEMU binary")
    ap.add_argument("--qemu-dir", default=None,
                    help="directory holding qemu-system-*; picks the right one")
    ap.add_argument("--kernel", default=None)
    ap.add_argument("--initrd", default=None)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    if args.kernel:
        TARGETS[args.arch]["kernel"] = args.kernel
    if args.initrd:
        TARGETS[args.arch]["initrd"] = args.initrd

    arches = list(TARGETS) if args.all else [args.arch]

    total = 0
    for i, arch in enumerate(arches):
        qemu = args.qemu
        if args.qemu_dir:
            name = TARGETS[arch]["qemu"]
            for cand in (name, name + ".exe"):
                path = os.path.join(args.qemu_dir, cand)
                if os.path.exists(path):
                    qemu = path
                    break
        total += run_one(arch, qemu, args.timeout, BASE_PORT + i)

    print()
    if total:
        print(f"{total} check(s) failed")
        return 1
    print(f"every check passed on {', '.join(arches)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
