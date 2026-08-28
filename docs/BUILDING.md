# Building and running RESENTMENT

Three ways, in order of how little you need installed.

---

## 1. The portable toolchain (nothing installed)

One download, no installer, no administrator rights, and it cross-compiles to
all three architectures:

```sh
make toolchain
```

That fetches [zig](https://ziglang.org) (a clang cross-compiler and an LLD
linker in one tarball) and, on Windows, NASM. It prints the two variables to
pass:

```sh
make ZIG=.toolchain/zig-x86_64-linux-0.15.2/zig \
     NASM=.toolchain/nasm-2.16.03/nasm
```

Set them once in your environment and `make` on its own works from then on.

---

## 2. Docker (nothing installed except Docker)

```sh
docker build -t resentment-build buildenv
docker run --rm -v "$PWD":/src -w /src resentment-build make check
docker run --rm -v "$PWD":/src -w /src resentment-build make all-arch
docker run --rm -v "$PWD":/src -w /src resentment-build make iso
```

The image has GCC cross compilers for all three architectures, NASM, GRUB,
xorriso and QEMU.

---

## 3. A native toolchain

```sh
make TOOLCHAIN=gcc
```

Looks for `<arch>-elf-gcc` first, then falls back to `<arch>-linux-gnu-gcc`,
which is what distributions actually package. With `-ffreestanding -nostdlib`
the second produces the same objects, and it is far easier to install than a
hand-built bare-metal toolchain.

You rarely have to ask for it. `TOOLCHAIN` defaults to `zig` when `zig` is on
`PATH` and to `gcc` when it is not, so a machine that has never run
`make toolchain` builds with what it has rather than failing with
`zig: not found`.

On Debian or Ubuntu:

```sh
sudo apt install build-essential nasm gcc-aarch64-linux-gnu \
                 gcc-riscv64-linux-gnu grub-pc-bin xorriso qemu-system
```

---

## Targets

| Command | What it does |
|---|---|
| `make` | build for x86_64 |
| `make ARCH=aarch64` / `ARCH=riscv64` | build for the others |
| `make all-arch` | build all three — the portability check |
| `make run` | boot under QEMU with no ISO needed |
| `make iso` / `make run-iso` | a GRUB-bootable ISO |
| `make run-script SCRIPT=/boot/bin/attest.she` | boot, run one script, power off |
| `make test` | the host test suite |
| `make qemu-test` | boot and drive the shell over a serial link |
| `make qemu-test-all` | every architecture, single core and on four |
| `make verify` | check the linked image is actually bootable |
| `make kaalka-check` | compare Kaalka against the reference implementation |
| `make check` | test + verify + kaalka-check |
| `make site` / `make site-serve` | build the documentation site from `docs/` |
| `make info` | show the resolved build configuration |
| `make clean` |  |

`make V=1` shows the full command lines.

---

## Running it

### The fast path

```sh
make run
```

Boots straight from the ELF with no bootloader. QEMU's `-kernel` loader
implements Multiboot 1 and refuses a 64-bit ELF, so the build produces a second
file — `resentment32.elf` — with the same segments at the same physical
addresses wrapped in 32-bit headers. Nothing about the kernel changes; the
64-bit virtual addresses it runs at are established by its own page tables a
few hundred instructions later, not by the ELF headers.

### From an ISO, the way real hardware boots it

```sh
make iso
qemu-system-x86_64 -cdrom dist/x86_64/resentment.iso -serial stdio -m 512M
```

GRUB has no 32-bit restriction and loads the Multiboot2 ELF directly.

### On the other architectures

```sh
make ARCH=aarch64 run
make ARCH=riscv64 run
```

Both boot the ELF directly on the QEMU `virt` machine. Note that both ports
currently run identity-mapped with the MMU off — see the status table in the
README.

---

## Kernel command line

Passed with `-append` under QEMU, or on the GRUB menu entry line.

| Option | Effect |
|---|---|
| `run=<path>` | run a SHE script before the prompt |
| `once` | power off after it, for scripted and CI runs |
| `allow=read,write,graph,...` | grant the shell session those permissions |
| `loglevel=<0-8>` | how much the kernel says |
| `memfab=<size>` | the memory fabric budget, accepts `K`, `M`, `G` |

```sh
make run-script SCRIPT=/boot/bin/attest.she
```

is exactly:

```sh
qemu-system-x86_64 -kernel dist/x86_64/resentment32.elf \
                   -initrd dist/x86_64/initrd.tar -m 512M -nographic \
                   -append "run=/boot/bin/attest.she once allow=all"
```

---

## The initial ramdisk

Everything in `user/` becomes `dist/<arch>/initrd.tar`, mounted at `/boot`:

```
user/etc/boot.she     runs before the first prompt
user/bin/*.she        run with .run /boot/bin/<name>.she
```

It is a plain USTAR archive, so you can inspect it with `tar tf`. The kernel
parses it without a decompressor, an index or an allocation, which is what lets
it be mounted before any driver exists.

---

## Testing

`make test` compiles the portable half of the kernel for the host and runs it
against a synthetic machine. It is the same source the kernel ships — the shim
in `tests/host/` supplies an arena, a clock and stubbed scheduling, and the
buddy allocator, slab heap, crypto, Kaalka, graph and SHE VM underneath are
real:

```
RESENTMENT host test suite

  strings
  printf
  128-bit division
  physical and heap allocator
  crypto
  calendar
  kaalka
  runtime graph
  federated memory fabric
  she language
  she sandbox
  she limits
  she diagnostics

1440 checks, 0 failures
```

`make qemu-test` is the integration test: it boots the kernel, connects to its
serial line over TCP, types at the prompt and checks the replies. Everything
from the interrupt controller to the SHE compiler has to work for one line to
come back correct.

```
reached the shell prompt
  ok    2 + 2  ->  4
  ok    say "hello from the shell"  ->  hello from the shell
  ok    xs |> filter(fun(n) -> n % 2 is 0) |> map(fun(n) -> n / 2) |> sum()  ->  35
  ok    read("/boot/etc/boot.she")  ->  --allow-read
  ok    .allow read  ->  granted
  ok    .run /boot/bin/attest.she  ->  comparing two hashes
  ...
all 28 checks passed
```

`make qemu-test-all` is the one to run before a release. It builds every
architecture and boots each of them twice — once single-core, once with
`-smp 4` — putting all 28 checks through each. The multiprocessor runs also
require the boot log to show every core reporting in, so a kernel that boots on
one core and wedges on four fails here rather than on somebody's laptop.

```
=== x86_64 ==================================================
  x86_64: all 28 checks passed
=== aarch64 =================================================
  aarch64: all 28 checks passed
=== riscv64 =================================================
  riscv64: all 28 checks passed
=== x86_64-smp ==============================================
  x86_64-smp: all 28 checks passed
=== aarch64-smp =============================================
  aarch64-smp: all 28 checks passed
=== riscv64-smp =============================================
  riscv64-smp: all 28 checks passed

every check passed on x86_64, aarch64, riscv64,
x86_64-smp, aarch64-smp, riscv64-smp
```

The last two checks run `/boot/bin/attest.she`, which is the scenario the whole
design exists for rather than a unit test: seal a snapshot of the machine, read
the clock angles Kaalka is keying from, prove the digest holds across pure
computation, then change the machine and watch its name change with it.

And the kernel runs its own self-tests on every boot, on the machine that is
about to be trusted:

```
[ 0.101597] selftest info    allocator self-test passed
[ 0.103153] kernel   info    crypto self-test passed: SHA-256, HMAC, ChaCha20, base64
[ 0.108596] kaalka   info    kaalka self-test passed: trig, transforms, seals, envelopes, replay
[ 0.112370] selftest info    graph self-test passed: digest stability and propagation
[ 0.116226] selftest info    capability self-test passed: type, rights, derive, revoke
[ 0.121352] infer    info    AI self-test passed: matmul, softmax, tensor lifecycle
[ 0.132227] selftest info    she self-test passed: arithmetic, loops, pipelines, sandbox, gas
[ 0.134773] selftest info    all 7 self-tests passed
```

---

## When something goes wrong

The panic handler prints the faulting address. Turn it into a name:

```sh
python tools/addr2sym.py dist/x86_64/resentment.elf 0xffffffff8011d32f
0xffffffff8011d32f  rk_accel_init+0xaf
```

Check that a linked image is actually loadable before blaming the kernel:

```sh
make verify
```

Watch what the CPU is doing:

```sh
qemu-system-x86_64 -kernel dist/x86_64/resentment32.elf -d int,cpu_reset -D qemu.log ...
```

Attach a debugger:

```sh
make debug          # QEMU waits on port 1234
gdb dist/x86_64/resentment.elf -ex 'target remote :1234'
```
