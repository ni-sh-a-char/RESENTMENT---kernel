# The AI subsystem

The premise: inference is not an application, it is a system resource, and the
kernel is the only place that can arbitrate it honestly.

A userspace runtime cannot see that two processes are thrashing the same
weights. It cannot preempt a half-finished decode to service a latency-critical
one. It cannot page an attention cache under memory pressure. And it cannot
stop a model it did not load from reading memory it was never granted. All four
are kernel jobs, and all four are why this subsystem exists.

---

## Inference as a scheduling class

A token-generation loop is neither of the things a normal scheduler knows how
to handle.

It is **not interactive**: it never sleeps waiting for a human, so a
sleep-credit heuristic gives it nothing and it gets treated as a CPU hog.

It is **not batch**: it has a rate the user can see, and missing it looks like
the machine stuttering.

It is a soft real-time stream with a deadline per token and a budget per
period, so `SCHED_INFERENCE` schedules it as one:

```c
sched_set_deadline(worker, 20 * RK_NS_PER_MS, 8 * RK_NS_PER_MS);
```

Eight milliseconds of budget every twenty. Enough to make progress on a token
stream, bounded enough that the rest of the system stays live. When a request
overruns its budget it is **demoted for the rest of its period** rather than
allowed to starve the UI — a slow answer beats a frozen machine, and that
demotion is the line between the two.

Admission control is real. `sched_set_deadline` sums the utilisation of every
deadline thread and refuses work past 80% of the available cores. A real-time
class that accepts everything is indistinguishable from having none.

Between tokens the worker yields. That single `sched_yield()` is the whole
reason this is a scheduling class rather than a library: a generation loop that
never yields makes the machine unusable, and one that yields every token does
not.

---

## Tensors as kernel objects

```c
struct rk_tensor {
    u8                dtype;
    u8                ndim;
    u64               shape[6];
    s64               stride[6];    /* in elements; 0 broadcasts */
    struct vm_object *store;        /* the backing pages */
    void             *data;
    struct rk_tensor *view_of;      /* zero-copy views */
};
```

Backed by a `vm_object`, which is what makes a tensor shareable: two tasks map
the same weights with no copy, and revoking one side is a capability operation
rather than a memory operation. Strides are in elements and may be zero, which
gives broadcasting for free and makes a transpose a view rather than a copy.

Supported types: `f32`, `f16`, `bf16`, `i32`, `i16`, `i8`, `u8`, `bool`, and
the block-quantised `q8_0`, `q4_0` and `q4_k` that quantised models are
actually distributed in.

---

## Operators

Scalar C, written to be obviously correct and to auto-vectorise: add, sub, mul,
div, matmul, matvec, relu, gelu, silu, tanh, sigmoid, softmax, layernorm,
rmsnorm, rope, attention, embed, argmax, quantize, dequantize, transpose.

The point is not to beat a hand-tuned BLAS. It is that the kernel can *see* the
work, account it, preempt it and place it. A GPU or NPU driver registers an
accelerator and these become the fallback for whatever it does not implement.

There is no libm, so the transcendentals are written out with their accuracy
stated: `exp` by range reduction plus a degree-5 minimax polynomial (relative
error below 1e-6), `tanh` from it with early saturation past |x| = 9, `sqrt` by
Newton from a bit-level initial guess, `log` from the exponent field plus an
atanh series, `sincos` sharing one range reduction.

Small decisions that matter to output quality are noted where they are made:
GELU uses the tanh approximation because that is what trained checkpoints were
trained against, and bf16 rounds to nearest even because truncation biases
every weight toward zero and that bias compounds across layers.

---

## Floating point in a kernel

Almost the entire kernel is compiled with the FPU and vector units **disabled**.
That is not conservatism. If the compiler is allowed to emit a vector
instruction anywhere, it will eventually emit one in an interrupt handler, and
the interrupted thread's register state is then silently corrupted. The
resulting bug is intermittent, data-dependent, and appears nowhere near its
cause.

So float lives in exactly one directory — `kernel/ai/`, compiled with SIMD on —
and every region that uses it is bracketed:

```c
rk_fpu_begin();     /* saves the interrupted state, disables preemption */
...
rk_fpu_end();
```

This is the same contract Linux's `kernel_fpu_begin` provides, for the same
reason.

The discipline is enforced by the build system, not by convention: the Makefile
has a separate rule for `kernel/ai/%.o` and every other translation unit is
compiled with `-mno-sse -mno-sse2 -mno-80387`. A stray float elsewhere is a
compile error, not a latent fault.

---

## Paged attention

An attention cache behaves exactly like memory. It is allocated in blocks, most
of it is cold, sequences that share a prefix hold identical contents, and the
worst case is far larger than the common case. Every one of those is something
a kernel memory manager already knows how to exploit — and a userspace runtime
cannot, which is why per-process cache reservation wastes most of the RAM on a
machine running several sessions.

So: pages of sixteen tokens, content-addressed by the digest of the layer, the
position and the contents, refcounted and shared.

```c
int rk_kv_append(struct rk_kv_cache *c, u32 layer,
                 const void *k, const void *v, u32 ntok);
int rk_kv_fork(struct rk_kv_cache *dst, struct rk_kv_cache *src, u32 upto);
void rk_kv_pressure_evict(u64 want_bytes);
```

Two sessions with the same system prompt store it once. That is not a micro
optimisation; on a machine serving several conversations it is most of the
cache.

`rk_kv_fork` makes speculative decoding and beam search affordable: a branch
costs a refcount, not a copy.

Under memory pressure, attention caches are the largest reclaimable allocation
on an inference machine, so they are the *first* thing asked to give memory
back rather than the last.

---

## Model provenance

Weights are loaded once for the machine and refcounted, so a second session
costs its KV cache and nothing else.

Every model carries a digest over every weight byte in canonical tensor order —
including tensor names and shapes, so renaming a tensor changes the digest — and
a Kaalka seal binding that digest to a time and an authority:

```c
int rk_model_verify(struct rk_model *m);   /* digest, then seal */
```

A model whose bytes changed after it was sealed **cannot be bound to an
inference capability**. `rk_infer_submit` checks `m->verified` at the gate,
before the compute loop, so the check cannot be skipped by anything that calls
the operators directly.

"Which model actually ran" is therefore an answerable question, which on a
conventional system it is not.

GGUF is parsed because that is what quantised models are distributed as. The
parser reads the header and the tensor table and maps the data in place; it
does not copy weights.

---

## Accelerators

One interface over CPU SIMD, a GPU queue, a phone NPU, a DSP, an FPGA or a
remote worker:

```c
struct rk_accel_ops {
    int  (*submit)(...);
    int  (*wait)(struct rk_accel *a, u64 fence, u64 timeout_ns);
    int  (*alloc)(struct rk_accel *a, size_t bytes, void **out, u64 *dev_addr);
    int  (*upload)(struct rk_accel *a, void *dev, const void *host, size_t n);
    bool (*supports)(struct rk_accel *a, enum rk_op op, enum rk_dtype dt);
};
```

`rk_accel_best(op, dtype)` picks the highest declared throughput that actually
supports the operator and is not saturated. The CPU backend is always present
and always last, so the AI subsystem works on a machine with no accelerator at
all — which is the machine most people will boot this on.

---

## Asking rather than deciding

Some kernel policy questions have no correct static answer: which page to
evict, where to place a thread, whether to admit a real-time task. A registered
advisor may answer them.

Three rules make that safe rather than clever:

- an advisor runs with a **hard deadline** and a late answer is discarded, not
  used — otherwise kernel behaviour would depend on how loaded the model was
- its answer is **clamped to the candidate set** the kernel offered, so it
  cannot name a page that was never a candidate however confident it is
- if there is no advisor, or it fails, the **deterministic heuristic runs**

```c
u64 rk_advice_ask(const struct rk_advice_query *q, u64 fallback);
```

An operating system may consult a model. It may never depend on one.

---

## Watching it work

```
resentment> .ai
inference submitted 0
inference completed 0
tokens generated    0
deadline misses     0
kv pages            0
kv shared pages     0
kv bytes            0 B
kv evictions        0

accelerator  kind    submitted  completed
cpu          0               3          3
```

The boot self-test exercises matmul against an identity, checks that softmax
sums to one, and verifies the tensor lifecycle, so a machine that reports the
AI subsystem as healthy has actually run arithmetic through it.
