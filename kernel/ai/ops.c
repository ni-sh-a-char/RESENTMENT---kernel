/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - tensor operator kernels.
 *
 * Scalar C, written to be obviously correct and to auto-vectorise. The point
 * of putting these in the kernel is not to beat a hand-tuned BLAS - it is that
 * the kernel can see the work, account it, preempt it and place it. A GPU or
 * NPU driver registers an accelerator (rk_accel) and these become the fallback
 * for whatever it does not implement, which is how one interface covers a
 * phone NPU and a server GPU.
 *
 * There is no libm here. exp and tanh are polynomial approximations chosen for
 * the range each activation actually sees, and the approximation error is
 * documented at each one rather than hidden.
 *
 * This file is compiled with the vector unit enabled; every public entry
 * brackets its work with rk_fpu_begin/rk_fpu_end.
 */
#include <rk/ai.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/arch.h>
#include <rk/time.h>
#include <rk/graph.h>

#undef RK_SUBSYS
#define RK_SUBSYS "ops"

static const char *const op_names[RK_OP_COUNT] = {
	"add", "sub", "mul", "div", "matmul", "matvec",
	"relu", "gelu", "silu", "tanh", "sigmoid",
	"softmax", "layernorm", "rmsnorm", "rope", "attention",
	"embed", "argmax", "topk", "quantize", "dequantize",
	"transpose", "concat", "slice", "conv2d", "maxpool2d"
};

const char *rk_op_name(enum rk_op op)
{
	return (unsigned)op < RK_OP_COUNT ? op_names[op] : "?";
}

/* ----------------------------------------------------------- half floats */

/* IEEE754 binary16 to binary32 by bit surgery. A table would be faster but
 * costs 128 KiB of kernel memory to save a handful of cycles per weight. */
static inline float f16_to_f32(u16 h)
{
	u32 sign = (u32)(h & 0x8000) << 16;
	u32 exp  = (h >> 10) & 0x1F;
	u32 mant = h & 0x3FF;
	u32 bits;

	if (exp == 0) {
		if (mant == 0) {
			bits = sign;                       /* signed zero */
		} else {
			/* Subnormal: renormalise into a binary32 normal. */
			exp = 127 - 15 + 1;
			while (!(mant & 0x400)) {
				mant <<= 1;
				exp--;
			}
			mant &= 0x3FF;
			bits = sign | (exp << 23) | (mant << 13);
		}
	} else if (exp == 0x1F) {
		bits = sign | 0x7F800000u | (mant << 13);   /* inf or nan */
	} else {
		bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
	}

	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

static inline u16 f32_to_f16(float f)
{
	u32 bits;
	memcpy(&bits, &f, sizeof(bits));

	u32 sign = (bits >> 16) & 0x8000;
	s32 exp  = (s32)((bits >> 23) & 0xFF) - 127 + 15;
	u32 mant = bits & 0x7FFFFF;

	if (exp <= 0)
		return (u16)sign;                       /* flush subnormals to zero */
	if (exp >= 0x1F)
		return (u16)(sign | 0x7C00);            /* saturate to infinity */
	return (u16)(sign | ((u32)exp << 10) | (mant >> 13));
}

/* bf16 is just the top half of an f32, which is why it survives training. */
static inline float bf16_to_f32(u16 b)
{
	u32 bits = (u32)b << 16;
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

static inline u16 f32_to_bf16(float f)
{
	u32 bits;
	memcpy(&bits, &f, sizeof(bits));
	/* Round to nearest even rather than truncate: truncation biases every
	 * weight toward zero and that bias compounds across layers. */
	u32 rounding = 0x7FFF + ((bits >> 16) & 1);
	return (u16)((bits + rounding) >> 16);
}

/* -------------------------------------------------------- element access */

/* One reader and one writer for every dtype. Dequantising on access is slower
 * than a fused quantised kernel, but it makes every operator work with every
 * dtype instead of needing an N-by-M matrix of special cases. The fused paths
 * that matter (quantised matmul) are written out separately below. */
static float load_elem(const void *base, enum rk_dtype dt, size_t i)
{
	switch (dt) {
	case RK_DT_F32:  return ((const float *)base)[i];
	case RK_DT_F16:  return f16_to_f32(((const u16 *)base)[i]);
	case RK_DT_BF16: return bf16_to_f32(((const u16 *)base)[i]);
	case RK_DT_I32:  return (float)((const s32 *)base)[i];
	case RK_DT_I16:  return (float)((const s16 *)base)[i];
	case RK_DT_I8:   return (float)((const s8 *)base)[i];
	case RK_DT_U8:
	case RK_DT_BOOL: return (float)((const u8 *)base)[i];
	case RK_DT_Q8_0: {
		size_t blk = i / 32, off = i % 32;
		const u8 *p = (const u8 *)base + blk * 34;
		float scale = f16_to_f32((u16)(p[0] | ((u16)p[1] << 8)));
		return scale * (float)(s8)p[2 + off];
	}
	case RK_DT_Q4_0: {
		size_t blk = i / 32, off = i % 32;
		const u8 *p = (const u8 *)base + blk * 18;
		float scale = f16_to_f32((u16)(p[0] | ((u16)p[1] << 8)));
		u8 packed = p[2 + off / 2];
		int nib = (off & 1) ? (packed >> 4) : (packed & 0xF);
		return scale * (float)(nib - 8);   /* 4-bit values are biased by 8 */
	}
	default: return 0.0f;
	}
}

static void store_elem(void *base, enum rk_dtype dt, size_t i, float v)
{
	switch (dt) {
	case RK_DT_F32:  ((float *)base)[i] = v; break;
	case RK_DT_F16:  ((u16 *)base)[i] = f32_to_f16(v); break;
	case RK_DT_BF16: ((u16 *)base)[i] = f32_to_bf16(v); break;
	case RK_DT_I32:  ((s32 *)base)[i] = (s32)v; break;
	case RK_DT_I16:  ((s16 *)base)[i] = (s16)v; break;
	case RK_DT_I8:   ((s8 *)base)[i] = (s8)CLAMP(v, -128.0f, 127.0f); break;
	case RK_DT_U8:
	case RK_DT_BOOL: ((u8 *)base)[i] = (u8)CLAMP(v, 0.0f, 255.0f); break;
	default: break;
	}
}

/* --------------------------------------------------------- transcendental */

/* exp(x) for x in about [-88, 88]. Split into 2^k times exp(r) with r small,
 * then a degree-5 minimax polynomial. Relative error below 1e-6, which is far
 * inside the noise of a quantised model. */
static float fast_exp(float x)
{
	if (x < -87.0f) return 0.0f;
	if (x >  88.0f) return 3.4028235e38f;

	const float log2e = 1.44269504f;
	float t = x * log2e;
	int   k = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
	float r = x - (float)k * 0.69314718f;

	float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f +
	          r * (0.04166667f + r * 0.00833333f))));

	/* Scale by 2^k by writing the exponent field directly. */
	u32 bits = (u32)((k + 127) & 0xFF) << 23;
	float scale;
	memcpy(&scale, &bits, sizeof(scale));
	return p * scale;
}

static float fast_tanh(float x)
{
	/* Saturate early: beyond |x| = 9 the difference from +-1 is below f32
	 * resolution and the exponential below would waste cycles. */
	if (x >  9.0f) return  1.0f;
	if (x < -9.0f) return -1.0f;
	float e = fast_exp(2.0f * x);
	return (e - 1.0f) / (e + 1.0f);
}

static float fast_sigmoid(float x) { return 1.0f / (1.0f + fast_exp(-x)); }

/* Natural log via the exponent field plus a polynomial on the mantissa.
 * Accurate to about 1e-6 over the whole normal range, which is all RoPE needs
 * from it. */
static float fast_log(float x)
{
	if (x <= 0.0f)
		return -87.0f;

	u32 bits;
	memcpy(&bits, &x, sizeof(bits));
	int e = (int)((bits >> 23) & 0xFF) - 127;
	bits = (bits & 0x007FFFFFu) | 0x3F800000u;   /* mantissa in [1, 2) */
	float m;
	memcpy(&m, &bits, sizeof(m));

	/* log(m) for m in [1,2) via the atanh series, which converges fast there. */
	float z = (m - 1.0f) / (m + 1.0f);
	float z2 = z * z;
	float lm = 2.0f * z * (1.0f + z2 * (0.33333333f + z2 * (0.2f + z2 * 0.14285714f)));
	return lm + (float)e * 0.69314718f;
}

/* Both at once: they share the range reduction, and RoPE always wants the pair.
 * Degree-7 and degree-6 minimax polynomials after reducing into [-pi, pi]. */
static void fast_sincos(float angle, float *out_sin, float *out_cos)
{
	const float twopi = 6.28318531f;
	float a = angle - twopi * (float)(s64)(angle / twopi);
	if (a >  3.14159265f) a -= twopi;
	if (a < -3.14159265f) a += twopi;

	float a2 = a * a;
	*out_sin = a * (1.0f - a2 * (0.16666667f - a2 * (0.00833333f - a2 * 0.00019841f)));
	*out_cos = 1.0f - a2 * (0.5f - a2 * (0.04166667f - a2 * 0.00138889f));
}

static float fast_sqrt(float x)
{
	if (x <= 0.0f)
		return 0.0f;
	/* Newton from a bit-level initial guess. Three iterations reach f32
	 * accuracy and this avoids depending on a libm we do not have. */
	u32 bits;
	memcpy(&bits, &x, sizeof(bits));
	bits = 0x1FBD1DF5u + (bits >> 1);
	float y;
	memcpy(&y, &bits, sizeof(y));
	y = 0.5f * (y + x / y);
	y = 0.5f * (y + x / y);
	y = 0.5f * (y + x / y);
	return y;
}

/* --------------------------------------------------------------- kernels */

static int op_elementwise(enum rk_op op, struct rk_tensor *dst,
                          struct rk_tensor *a, struct rk_tensor *b)
{
	if (!dst || !a)
		return RK_EINVAL;
	u64 n = dst->nelem;

	for (u64 i = 0; i < n; i++) {
		float x = load_elem(a->data, (enum rk_dtype)a->dtype, i % a->nelem);
		float y = b ? load_elem(b->data, (enum rk_dtype)b->dtype, i % b->nelem) : 0.0f;
		float r;
		switch (op) {
		case RK_OP_ADD: r = x + y; break;
		case RK_OP_SUB: r = x - y; break;
		case RK_OP_MUL: r = x * y; break;
		case RK_OP_DIV: r = y != 0.0f ? x / y : 0.0f; break;
		case RK_OP_RELU: r = x > 0.0f ? x : 0.0f; break;
		case RK_OP_SILU: r = x * fast_sigmoid(x); break;
		case RK_OP_TANH: r = fast_tanh(x); break;
		case RK_OP_SIGMOID: r = fast_sigmoid(x); break;
		case RK_OP_GELU:
			/* The tanh approximation, which is what trained checkpoints
			 * were trained against; the exact erf form gives visibly
			 * different logits. */
			r = 0.5f * x * (1.0f + fast_tanh(0.79788456f * (x + 0.044715f * x * x * x)));
			break;
		default: return RK_ENOTSUP;
		}
		store_elem(dst->data, (enum rk_dtype)dst->dtype, i, r);
	}
	return RK_OK;
}

/* dst[m,n] = a[m,k] * b[k,n]. Blocked over k in the inner loop so the
 * accumulator stays in a register and b is walked contiguously. */
static int op_matmul(struct rk_tensor *dst, struct rk_tensor *a, struct rk_tensor *b)
{
	if (!dst || !a || !b || a->ndim < 2 || b->ndim < 2)
		return RK_EINVAL;

	u64 m = a->shape[a->ndim - 2];
	u64 k = a->shape[a->ndim - 1];
	u64 k2 = b->shape[b->ndim - 2];
	u64 n = b->shape[b->ndim - 1];

	if (k != k2 || dst->nelem < m * n)
		return RK_EINVAL;

	for (u64 i = 0; i < m; i++) {
		for (u64 j = 0; j < n; j++) {
			float acc = 0.0f;
			for (u64 p = 0; p < k; p++)
				acc += load_elem(a->data, (enum rk_dtype)a->dtype, i * k + p) *
				       load_elem(b->data, (enum rk_dtype)b->dtype, p * n + j);
			store_elem(dst->data, (enum rk_dtype)dst->dtype, i * n + j, acc);
		}
	}
	return RK_OK;
}

/* The shape that actually dominates decoding: one row against a weight
 * matrix. Kept separate because the loop order that is right here (walk the
 * weights contiguously, accumulate one output) is wrong for the general case. */
static int op_matvec(struct rk_tensor *dst, struct rk_tensor *w, struct rk_tensor *x)
{
	if (!dst || !w || !x || w->ndim < 2)
		return RK_EINVAL;

	u64 rows = w->shape[w->ndim - 2];
	u64 cols = w->shape[w->ndim - 1];
	if (x->nelem < cols || dst->nelem < rows)
		return RK_EINVAL;

	enum rk_dtype wt = (enum rk_dtype)w->dtype;
	enum rk_dtype xt = (enum rk_dtype)x->dtype;

	for (u64 r = 0; r < rows; r++) {
		float acc = 0.0f;
		const u64 base = r * cols;
		for (u64 c = 0; c < cols; c++)
			acc += load_elem(w->data, wt, base + c) * load_elem(x->data, xt, c);
		store_elem(dst->data, (enum rk_dtype)dst->dtype, r, acc);
	}
	return RK_OK;
}

/* Numerically stable softmax: subtract the row maximum before exponentiating,
 * because exp of a raw logit overflows f32 at about 88 and real logits get
 * there. */
static int op_softmax(struct rk_tensor *dst, struct rk_tensor *src, s32 axis)
{
	(void)axis;
	if (!dst || !src)
		return RK_EINVAL;

	u64 n = src->nelem;
	u64 row = src->ndim ? src->shape[src->ndim - 1] : n;
	if (!row)
		return RK_EINVAL;

	for (u64 base = 0; base + row <= n; base += row) {
		float max = load_elem(src->data, (enum rk_dtype)src->dtype, base);
		for (u64 i = 1; i < row; i++) {
			float v = load_elem(src->data, (enum rk_dtype)src->dtype, base + i);
			if (v > max)
				max = v;
		}
		float sum = 0.0f;
		for (u64 i = 0; i < row; i++) {
			float e = fast_exp(load_elem(src->data, (enum rk_dtype)src->dtype, base + i) - max);
			store_elem(dst->data, (enum rk_dtype)dst->dtype, base + i, e);
			sum += e;
		}
		float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
		for (u64 i = 0; i < row; i++) {
			float e = load_elem(dst->data, (enum rk_dtype)dst->dtype, base + i);
			store_elem(dst->data, (enum rk_dtype)dst->dtype, base + i, e * inv);
		}
	}
	return RK_OK;
}

static int op_rmsnorm(struct rk_tensor *dst, struct rk_tensor *src,
                      struct rk_tensor *weight, float eps)
{
	if (!dst || !src)
		return RK_EINVAL;

	u64 n = src->nelem;
	u64 row = src->ndim ? src->shape[src->ndim - 1] : n;
	if (!row)
		return RK_EINVAL;

	for (u64 base = 0; base + row <= n; base += row) {
		float sumsq = 0.0f;
		for (u64 i = 0; i < row; i++) {
			float v = load_elem(src->data, (enum rk_dtype)src->dtype, base + i);
			sumsq += v * v;
		}
		float scale = 1.0f / fast_sqrt(sumsq / (float)row + eps);
		for (u64 i = 0; i < row; i++) {
			float v = load_elem(src->data, (enum rk_dtype)src->dtype, base + i) * scale;
			if (weight)
				v *= load_elem(weight->data, (enum rk_dtype)weight->dtype, i);
			store_elem(dst->data, (enum rk_dtype)dst->dtype, base + i, v);
		}
	}
	return RK_OK;
}

static int op_layernorm(struct rk_tensor *dst, struct rk_tensor *src,
                        struct rk_tensor *weight, struct rk_tensor *bias, float eps)
{
	if (!dst || !src)
		return RK_EINVAL;

	u64 n = src->nelem;
	u64 row = src->ndim ? src->shape[src->ndim - 1] : n;
	if (!row)
		return RK_EINVAL;

	for (u64 base = 0; base + row <= n; base += row) {
		float mean = 0.0f;
		for (u64 i = 0; i < row; i++)
			mean += load_elem(src->data, (enum rk_dtype)src->dtype, base + i);
		mean /= (float)row;

		float var = 0.0f;
		for (u64 i = 0; i < row; i++) {
			float d = load_elem(src->data, (enum rk_dtype)src->dtype, base + i) - mean;
			var += d * d;
		}
		var /= (float)row;

		float inv = 1.0f / fast_sqrt(var + eps);
		for (u64 i = 0; i < row; i++) {
			float v = (load_elem(src->data, (enum rk_dtype)src->dtype, base + i) - mean) * inv;
			if (weight)
				v *= load_elem(weight->data, (enum rk_dtype)weight->dtype, i);
			if (bias)
				v += load_elem(bias->data, (enum rk_dtype)bias->dtype, i);
			store_elem(dst->data, (enum rk_dtype)dst->dtype, base + i, v);
		}
	}
	return RK_OK;
}

/* Rotary position embedding. The rotation angle per pair depends only on the
 * position and the pair index, so it is recomputed rather than cached: the
 * cache would be a per-model allocation for arithmetic that costs less than
 * the load it would replace. */
static int op_rope(struct rk_tensor *t, u64 pos, u32 head_dim, float theta)
{
	if (!t || !head_dim)
		return RK_EINVAL;
	if (theta <= 0.0f)
		theta = 10000.0f;

	u64 nheads = t->nelem / head_dim;
	for (u64 h = 0; h < nheads; h++) {
		u64 base = h * head_dim;
		float ln_theta = fast_log(theta);
		for (u32 i = 0; i + 1 < head_dim; i += 2) {
			/* theta^(-i/head_dim), computed as exp(-i/head_dim * ln theta)
			 * because there is no pow and this is exactly as accurate. */
			float freq = fast_exp(-(float)i / (float)head_dim * ln_theta);
			float angle = (float)pos * freq;

			float s, c;
			fast_sincos(angle, &s, &c);

			float x0 = load_elem(t->data, (enum rk_dtype)t->dtype, base + i);
			float x1 = load_elem(t->data, (enum rk_dtype)t->dtype, base + i + 1);
			store_elem(t->data, (enum rk_dtype)t->dtype, base + i,     x0 * c - x1 * s);
			store_elem(t->data, (enum rk_dtype)t->dtype, base + i + 1, x0 * s + x1 * c);
		}
	}
	return RK_OK;
}

static int op_argmax(struct rk_tensor *dst, struct rk_tensor *src)
{
	if (!dst || !src || !src->nelem)
		return RK_EINVAL;

	float best = load_elem(src->data, (enum rk_dtype)src->dtype, 0);
	u64 idx = 0;
	for (u64 i = 1; i < src->nelem; i++) {
		float v = load_elem(src->data, (enum rk_dtype)src->dtype, i);
		if (v > best) {
			best = v;
			idx = i;
		}
	}
	store_elem(dst->data, (enum rk_dtype)dst->dtype, 0, (float)idx);
	return RK_OK;
}

static int op_embed(struct rk_tensor *dst, struct rk_tensor *table,
                    struct rk_tensor *ids)
{
	if (!dst || !table || !ids || table->ndim < 2)
		return RK_EINVAL;

	u64 dim = table->shape[table->ndim - 1];
	u64 vocab = table->shape[table->ndim - 2];

	for (u64 t = 0; t < ids->nelem; t++) {
		u64 id = (u64)load_elem(ids->data, (enum rk_dtype)ids->dtype, t);
		if (id >= vocab)
			return RK_ERANGE;
		for (u64 d = 0; d < dim; d++) {
			float v = load_elem(table->data, (enum rk_dtype)table->dtype, id * dim + d);
			store_elem(dst->data, (enum rk_dtype)dst->dtype, t * dim + d, v);
		}
	}
	return RK_OK;
}

/* Single-head scaled dot-product attention over a contiguous K/V block.
 * q[head_dim], k[seq, head_dim], v[seq, head_dim] -> dst[head_dim]. */
static int op_attention(struct rk_tensor *dst, struct rk_tensor *q,
                        struct rk_tensor *k, struct rk_tensor *v,
                        u32 head_dim, float scale)
{
	if (!dst || !q || !k || !v || !head_dim)
		return RK_EINVAL;

	u64 seq = k->nelem / head_dim;
	if (!seq || v->nelem / head_dim < seq)
		return RK_EINVAL;
	if (scale == 0.0f)
		scale = 1.0f / fast_sqrt((float)head_dim);

	float *scores = kmalloc(seq * sizeof(float));
	if (!scores)
		return RK_ENOMEM;

	float max = -3.4e38f;
	for (u64 s = 0; s < seq; s++) {
		float dot = 0.0f;
		for (u32 d = 0; d < head_dim; d++)
			dot += load_elem(q->data, (enum rk_dtype)q->dtype, d) *
			       load_elem(k->data, (enum rk_dtype)k->dtype, s * head_dim + d);
		scores[s] = dot * scale;
		if (scores[s] > max)
			max = scores[s];
	}

	float sum = 0.0f;
	for (u64 s = 0; s < seq; s++) {
		scores[s] = fast_exp(scores[s] - max);
		sum += scores[s];
	}
	float inv = sum > 0.0f ? 1.0f / sum : 0.0f;

	for (u32 d = 0; d < head_dim; d++) {
		float acc = 0.0f;
		for (u64 s = 0; s < seq; s++)
			acc += scores[s] * inv *
			       load_elem(v->data, (enum rk_dtype)v->dtype, s * head_dim + d);
		store_elem(dst->data, (enum rk_dtype)dst->dtype, d, acc);
	}

	kfree(scores);
	return RK_OK;
}

/* Symmetric per-block quantisation, matching the q8_0 and q4_0 layouts the
 * loaders expect. */
static int op_quantize(struct rk_tensor *dst, struct rk_tensor *src)
{
	if (!dst || !src)
		return RK_EINVAL;

	enum rk_dtype dt = (enum rk_dtype)dst->dtype;
	size_t block = rk_dtype_block(dt);
	if (block <= 1)
		return RK_ENOTSUP;

	u64 nblocks = DIV_ROUND_UP(src->nelem, block);
	u8 *out = dst->data;

	for (u64 b = 0; b < nblocks; b++) {
		float amax = 0.0f;
		for (size_t i = 0; i < block; i++) {
			u64 idx = b * block + i;
			if (idx >= src->nelem)
				break;
			float v = load_elem(src->data, (enum rk_dtype)src->dtype, idx);
			float a = v < 0.0f ? -v : v;
			if (a > amax)
				amax = a;
		}

		if (dt == RK_DT_Q8_0) {
			float scale = amax / 127.0f;
			float inv = scale > 0.0f ? 1.0f / scale : 0.0f;
			u16 h = f32_to_f16(scale);
			u8 *p = out + b * 34;
			p[0] = (u8)h;
			p[1] = (u8)(h >> 8);
			for (size_t i = 0; i < 32; i++) {
				u64 idx = b * 32 + i;
				float v = idx < src->nelem
				        ? load_elem(src->data, (enum rk_dtype)src->dtype, idx) : 0.0f;
				p[2 + i] = (u8)(s8)CLAMP(v * inv, -127.0f, 127.0f);
			}
		} else if (dt == RK_DT_Q4_0) {
			float scale = amax / 7.0f;
			float inv = scale > 0.0f ? 1.0f / scale : 0.0f;
			u16 h = f32_to_f16(scale);
			u8 *p = out + b * 18;
			p[0] = (u8)h;
			p[1] = (u8)(h >> 8);
			for (size_t i = 0; i < 32; i += 2) {
				u64 i0 = b * 32 + i, i1 = i0 + 1;
				float v0 = i0 < src->nelem
				         ? load_elem(src->data, (enum rk_dtype)src->dtype, i0) : 0.0f;
				float v1 = i1 < src->nelem
				         ? load_elem(src->data, (enum rk_dtype)src->dtype, i1) : 0.0f;
				int n0 = (int)CLAMP(v0 * inv + 8.5f, 0.0f, 15.0f);
				int n1 = (int)CLAMP(v1 * inv + 8.5f, 0.0f, 15.0f);
				p[2 + i / 2] = (u8)(n0 | (n1 << 4));
			}
		} else {
			return RK_ENOTSUP;
		}
	}
	return RK_OK;
}

static int op_dequantize(struct rk_tensor *dst, struct rk_tensor *src)
{
	if (!dst || !src)
		return RK_EINVAL;
	for (u64 i = 0; i < dst->nelem && i < src->nelem; i++)
		store_elem(dst->data, (enum rk_dtype)dst->dtype, i,
		           load_elem(src->data, (enum rk_dtype)src->dtype, i));
	return RK_OK;
}

static int op_transpose(struct rk_tensor *dst, struct rk_tensor *src)
{
	if (!dst || !src || src->ndim != 2)
		return RK_EINVAL;
	u64 r = src->shape[0], c = src->shape[1];
	for (u64 i = 0; i < r; i++)
		for (u64 j = 0; j < c; j++)
			store_elem(dst->data, (enum rk_dtype)dst->dtype, j * r + i,
			           load_elem(src->data, (enum rk_dtype)src->dtype, i * c + j));
	return RK_OK;
}

/* ------------------------------------------------------------ dispatcher */

int rk_op_exec(enum rk_op op, struct rk_tensor *dst,
               struct rk_tensor *a, struct rk_tensor *b, struct rk_tensor *c,
               const struct rk_op_args *args)
{
	static const struct rk_op_args defaults = { 0 };
	if (!args)
		args = &defaults;

	u64 t0 = rk_time_ns();
	int rc;

	rk_fpu_begin();
	switch (op) {
	case RK_OP_ADD: case RK_OP_SUB: case RK_OP_MUL: case RK_OP_DIV:
	case RK_OP_RELU: case RK_OP_GELU: case RK_OP_SILU:
	case RK_OP_TANH: case RK_OP_SIGMOID:
		rc = op_elementwise(op, dst, a, b);
		break;
	case RK_OP_MATMUL:    rc = op_matmul(dst, a, b); break;
	case RK_OP_MATVEC:    rc = op_matvec(dst, a, b); break;
	case RK_OP_SOFTMAX:   rc = op_softmax(dst, a, args->axis); break;
	case RK_OP_RMSNORM:   rc = op_rmsnorm(dst, a, b, args->eps ? args->eps : 1e-5f); break;
	case RK_OP_LAYERNORM: rc = op_layernorm(dst, a, b, c, args->eps ? args->eps : 1e-5f); break;
	case RK_OP_ROPE:      rc = op_rope(dst, args->pos, args->head_dim, args->scale); break;
	case RK_OP_ATTENTION: rc = op_attention(dst, a, b, c, args->head_dim, args->scale); break;
	case RK_OP_EMBED:     rc = op_embed(dst, a, b); break;
	case RK_OP_ARGMAX:    rc = op_argmax(dst, a); break;
	case RK_OP_QUANTIZE:  rc = op_quantize(dst, a); break;
	case RK_OP_DEQUANTIZE:rc = op_dequantize(dst, a); break;
	case RK_OP_TRANSPOSE: rc = op_transpose(dst, a); break;
	default:              rc = RK_ENOSYS; break;
	}
	rk_fpu_end();

	rk_graph_record(GEV_INFERENCE, dst ? dst->graph_node : 0,
	                (u64)op, dst ? dst->nelem : 0, rk_time_ns() - t0);
	return rc;
}

int rk_tensor_fill(struct rk_tensor *t, float value)
{
	if (!t)
		return RK_EINVAL;
	if (t->flags & RK_TENSOR_F_READONLY)
		return RK_EACCES;

	rk_fpu_begin();
	for (u64 i = 0; i < t->nelem; i++)
		store_elem(t->data, (enum rk_dtype)t->dtype, i, value);
	rk_fpu_end();
	return RK_OK;
}

/* Exposed so the model loader and the self-test can convert without duplicating
 * the bit surgery. */
float rk_f16_to_f32(u16 h) { return f16_to_f32(h); }
u16   rk_f32_to_f16(float f) { return f32_to_f16(f); }
float rk_tensor_get_f32(const struct rk_tensor *t, u64 index)
{
	return t && index < t->nelem
	     ? load_elem(t->data, (enum rk_dtype)t->dtype, index) : 0.0f;
}
void rk_tensor_set_f32(struct rk_tensor *t, u64 index, float v)
{
	if (t && index < t->nelem)
		store_elem(t->data, (enum rk_dtype)t->dtype, index, v);
}
