# The runtime graph

Modelled on [WebWeaveX](https://github.com/ni-sh-a-char/WebWeaveX), which
captures a running system as a deterministic graph with stable node identities
so that an agent can reason about it. WebWeaveX does this for web applications.
RESENTMENT does it for the operating system itself.

---

## The problem

Ask a conventional machine "are you in the state I attested?" and there is no
answer. You can scrape `ps`, `lsof`, `/proc`, a package manifest and a
configuration file, and you still have a pile of text with no name. System
state is not addressable, so there is nothing to compare, nothing to sign, and
nothing to reproduce.

A kernel already knows every object it created. The only thing missing is a
naming scheme.

---

## Node identity

Every kernel object is a node: task, thread, capability, mapping, endpoint,
channel, file, device, model, tensor, script. Each carries a SHA-256 digest
over a canonical encoding of:

1. its kind
2. its label
3. every attribute, in sorted key order, with its type tag
4. the digest of every child, in ascending digest order
5. its outgoing edges

Which makes the graph a Merkle DAG and the whole machine one root hash.

**What is deliberately excluded is the important part**: timestamps, object
ids, version counters and pointers. Two machines that booted at different times
but reached the same configuration must hash identically, or comparing across
machines is useless. The digest names the *state*, not the history of getting
there.

Attribute order does not matter, because the encoding sorts keys. Child order
does not matter, because children fold in digest order. Identical subtrees are
emitted once per occurrence, so multiplicity is preserved — two identical
threads are not the same as one.

---

## What that buys

### Introspection

An agent reads typed state through one interface instead of scraping a dozen
tools:

```
resentment> .ls /graph
  graph  digest      the SHA-256 merkle root of the entire system
  graph  tree        every kernel object as an indented tree
  graph  json        the same, deterministically
  graph  canon       the canonical per-node digest listing
  graph  dot         graphviz source
  graph  events      the causal event log
  graph  threads     every thread, its class and its CPU time
  graph  meminfo     physical and heap memory
  graph  kaalka      temporal keying and the live clock angles
  graph  ai          inference queue, KV cache, accelerators
  ...
```

Everything there is generated when read, so nothing can be stale.

### Diffing

Because each line of a canonical export begins with a subtree digest, an
unchanged subtree is one identical line and is skipped wholesale. Comparing two
machines, or one machine at two instants, costs a walk of what actually
differs.

### Attestation

A snapshot is a canonical export plus a Kaalka seal over its digest:

```
resentment> .snapshot
snapshot 42
  root   05984d529c7070be15473a196de9c1e6f2847fa7dd3acc7ce732ef25b5174fcf
  nodes  15
  bytes  1148
  sealed until 1787896745
```

It answers what the machine looked like *and when*, and the seal makes both
unforgeable and un-backdatable.

### Replay

Events are recorded in causal order — sequence, monotonic timestamp, CPU,
thread, and up to three payload words:

```
resentment> .events
     seq        mono_ns cpu    tid kind           a b c
     412       88213004   0      1 syscall        128 3 0
     413       88219871   0      1 cap_check      7 1 0
     414       88231402   0      1 fault          7f2c1000 2 ...
```

During replay the clock is virtual and the recorded timeline is played back, so
a replayed system observes the timestamps the recorded one did. A divergence
from the recorded digests is reported as `RK_EDETERM` rather than silently
producing a different run.

That is what turns "it crashed on a customer machine" into a reproducible run:
the panic handler prints the root digest and the tail of the causal log,

```
*** RESENTMENT KERNEL PANIC ***
graph   root=86af7d043177f20c7ee9d553488db78aa94ee89c2b106cbd2304de90c33c00ed
        replay with: resentment --replay <snapshot> --expect 86af7d043177f20c
```

and the digest identifies exactly which state to replay into.

---

## Making it affordable

This runs inside a kernel, so cost control is not an afterthought.

**Digests are lazy.** Creating a node hashes nothing. A digest is computed the
first time somebody asks and cached until the subtree changes.

**Invalidation walks parents only.** A change deep in the tree costs a walk to
the root, not a rehash of the tree.

**The event ring is fixed-size with a lock-free producer.** Recording from an
interrupt handler is an atomic increment and a few stores. A tracing facility
that has to be turned off is not a tracing facility.

**High-rate event kinds are off by default.** Context switches, interrupts and
capability checks can be enabled at runtime when you want them and cost nothing
when you do not.

**The child fold uses selection sort over the list, not an array.** The digest
function recurses, and a stack array would multiply by tree depth. It is marked
in the source as a deliberate ceiling with the upgrade path named.

---

## The federated memory fabric

WebWeaveX merges execution histories across turns into a deterministic
key/value store. RESENTMENT applies the same idea across *boots*.

```she
remember("boot_count", 7)
let n = recall("boot_count")
```

Entries are content-addressed, so a merge is idempotent by construction: the
same content always lands on the same key, and merging a fabric into itself
changes nothing. Two machines can exchange fabrics with no conflict resolution
policy, because there are no conflicts to resolve.

It is bounded — this is kernel memory — and evicts least-recently-used rather
than growing without limit.

The fabric is itself a graph node, so writing to it moves the root digest. That
is not decoration: if changing the machine did not change its hash, the hash
would stop meaning anything.

```she
let before = digest()
remember("note", "hello")
let after = digest()
# before is not after
```

Meanwhile pure computation does *not* move the digest, and that is the property
rather than a limitation — the hash names the state of the machine, not the
noise of whatever happened to be running.
