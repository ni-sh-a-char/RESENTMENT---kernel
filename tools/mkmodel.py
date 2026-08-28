#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Synthesise a tiny GGUF model so the inference path can be exercised.

The weights are pseudo-random and the tokens it produces mean nothing. That is
the point: this is not a claim about model quality, it is a fixture for the
*systems* claim - that the kernel can load a model, run a real transformer
forward pass through its own operators, and schedule the result as a
soft real-time stream.

Deterministic, so two builds produce byte-identical files and the model digest
in the runtime graph is stable. Small, because it is linked into the kernel
image on the architectures whose firmware cannot pass a module.

  python tools/mkmodel.py user/bin/tiny.gguf
"""

import os
import struct
import sys

ARCH        = "llama"
N_LAYERS    = 2
EMBED_DIM   = 32
N_HEADS     = 4
N_KV_HEADS  = 4
FFN_DIM     = 64
VOCAB       = 64
CONTEXT     = 128
HEAD_DIM    = EMBED_DIM // N_HEADS
ROPE_THETA  = 10000.0
NORM_EPS    = 1e-5

GGUF_MAGIC = 0x46554747
GGUF_VER   = 3

# GGUF value type tags
T_U32, T_F32, T_STRING = 4, 6, 8
GGML_F32 = 0


def lcg(seed):
    """A 64-bit linear congruential generator. Written out rather than using
    Python's random module so the file is identical on every interpreter and
    every platform, which is what makes the model digest reproducible."""
    state = seed & 0xFFFFFFFFFFFFFFFF
    while True:
        state = (state * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        yield (state >> 33) / float(1 << 31)          # [0, 2)


def weights(name, n, scale):
    """Small values centred on zero. Kept small deliberately: a random network
    with large weights saturates and every position produces the same token,
    which would make the demo look broken when it is only meaningless."""
    seed = 0x9E3779B97F4A7C15
    for ch in name.encode():
        seed = (seed * 1099511628211 + ch) & 0xFFFFFFFFFFFFFFFF
    gen = lcg(seed)
    return b"".join(struct.pack("<f", (next(gen) - 1.0) * scale) for _ in range(n))


def ones(n):
    return struct.pack("<f", 1.0) * n


def s(text):
    b = text.encode()
    return struct.pack("<Q", len(b)) + b


def kv_u32(key, value):
    return s(key) + struct.pack("<I", T_U32) + struct.pack("<I", value)


def kv_f32(key, value):
    return s(key) + struct.pack("<I", T_F32) + struct.pack("<f", value)


def kv_str(key, value):
    return s(key) + struct.pack("<I", T_STRING) + s(value)


def build():
    # (name, ggml dims fastest-first, payload)
    #
    # GGUF lists dimensions fastest-varying first, and the kernel's parser
    # reverses them into row-major. So a weight the kernel wants as
    # [out, in] is written here as {in, out}.
    tensors = []

    def add(name, dims, data):
        tensors.append((name, dims, data))

    add("token_embd.weight", (EMBED_DIM, VOCAB),
        weights("token_embd", VOCAB * EMBED_DIM, 0.30))

    for i in range(N_LAYERS):
        p = "blk.%d." % i
        add(p + "attn_norm.weight", (EMBED_DIM,), ones(EMBED_DIM))
        for w in ("attn_q", "attn_k", "attn_v", "attn_output"):
            add(p + w + ".weight", (EMBED_DIM, EMBED_DIM),
                weights(p + w, EMBED_DIM * EMBED_DIM, 0.14))
        add(p + "ffn_norm.weight", (EMBED_DIM,), ones(EMBED_DIM))
        add(p + "ffn_gate.weight", (EMBED_DIM, FFN_DIM),
            weights(p + "ffn_gate", EMBED_DIM * FFN_DIM, 0.14))
        add(p + "ffn_up.weight", (EMBED_DIM, FFN_DIM),
            weights(p + "ffn_up", EMBED_DIM * FFN_DIM, 0.14))
        add(p + "ffn_down.weight", (FFN_DIM, EMBED_DIM),
            weights(p + "ffn_down", FFN_DIM * EMBED_DIM, 0.14))

    add("output_norm.weight", (EMBED_DIM,), ones(EMBED_DIM))
    add("output.weight", (EMBED_DIM, VOCAB),
        weights("output", VOCAB * EMBED_DIM, 0.30))

    meta = b"".join([
        kv_str(ARCH + ".name", "resentment-tiny"),
        kv_str("general.architecture", ARCH),
        kv_u32(ARCH + ".block_count", N_LAYERS),
        kv_u32(ARCH + ".embedding_length", EMBED_DIM),
        kv_u32(ARCH + ".attention.head_count", N_HEADS),
        kv_u32(ARCH + ".attention.head_count_kv", N_KV_HEADS),
        kv_u32(ARCH + ".feed_forward_length", FFN_DIM),
        kv_u32(ARCH + ".context_length", CONTEXT),
        kv_u32(ARCH + ".vocab_size", VOCAB),
        kv_f32(ARCH + ".attention.layer_norm_rms_epsilon", NORM_EPS),
        kv_f32(ARCH + ".rope.freq_base", ROPE_THETA),
    ])
    nmeta = 11

    # Tensor data is laid out back to back, each aligned to 32 bytes, and the
    # offsets are relative to the start of the data section.
    align = 32
    blobs, infos, off = [], [], 0
    for name, dims, data in tensors:
        pad = (-off) % align
        if pad:
            blobs.append(b"\0" * pad)
            off += pad
        infos.append((name, dims, off))
        blobs.append(data)
        off += len(data)

    info_bytes = b"".join(
        s(name) + struct.pack("<I", len(dims)) +
        b"".join(struct.pack("<Q", d) for d in dims) +
        struct.pack("<I", GGML_F32) + struct.pack("<Q", offset)
        for name, dims, offset in infos)

    header = (struct.pack("<I", GGUF_MAGIC) +
              struct.pack("<I", GGUF_VER) +
              struct.pack("<Q", len(tensors)) +
              struct.pack("<Q", nmeta) +
              meta + info_bytes)

    pad = (-len(header)) % align
    return header + b"\0" * pad + b"".join(blobs), len(tensors)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "user/bin/tiny.gguf"
    data, ntensors = build()
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(data)
    print("  MODEL   %s  %d tensors, %d layers, dim %d, vocab %d, %.1f KiB"
          % (out, ntensors, N_LAYERS, EMBED_DIM, VOCAB, len(data) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
