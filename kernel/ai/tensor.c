/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - tensors as kernel objects.
 *
 * A tensor is backed by a vm_object, which is what makes it shareable: two
 * tasks can map the same weights with no copy, and revoking one side is a
 * capability operation rather than a memory operation. That is the difference
 * between the kernel owning inference and a library owning it.
 *
 * Strides are in elements and may be zero, which gives broadcasting for free
 * and makes a transpose a view rather than a copy.
 *
 * This directory is compiled with the vector unit enabled. Every entry point
 * that touches float must be inside rk_fpu_begin/rk_fpu_end.
 */
#include <rk/ai.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/graph.h>
#include <rk/printf.h>
#include <rk/arch.h>

#undef RK_SUBSYS
#define RK_SUBSYS "tensor"

static rk_id_t next_tensor_id = 1;
static struct kmem_cache *tensor_cache;

/* Quantised formats store a block of values plus a scale, so the "size of one
 * element" question has two answers and both are needed. */
struct dtype_info {
	const char *name;
	u8  elem_bits;    /* 0 for blocked formats */
	u16 block;        /* elements per block, 1 when dense */
	u16 block_bytes;  /* bytes per block */
};

static const struct dtype_info dtypes[RK_DT_COUNT] = {
	[RK_DT_INVALID] = { "invalid", 0,  1,  0 },
	[RK_DT_F32]     = { "f32",    32,  1,  4 },
	[RK_DT_F16]     = { "f16",    16,  1,  2 },
	[RK_DT_BF16]    = { "bf16",   16,  1,  2 },
	[RK_DT_I32]     = { "i32",    32,  1,  4 },
	[RK_DT_I16]     = { "i16",    16,  1,  2 },
	[RK_DT_I8]      = { "i8",      8,  1,  1 },
	[RK_DT_U8]      = { "u8",      8,  1,  1 },
	/* q8_0: 32 int8 values plus one f16 scale = 34 bytes per 32 values. */
	[RK_DT_Q8_0]    = { "q8_0",    0, 32, 34 },
	/* q4_0: 32 values packed two per byte plus one f16 scale = 18 bytes. */
	[RK_DT_Q4_0]    = { "q4_0",    0, 32, 18 },
	/* q4_k: 256 values, 12 bytes of packed scales, two f16 super-scales. */
	[RK_DT_Q4_K]    = { "q4_k",    0, 256, 144 },
	[RK_DT_BOOL]    = { "bool",    8,  1,  1 },
};

size_t rk_dtype_size(enum rk_dtype dt)
{
	if ((unsigned)dt >= RK_DT_COUNT)
		return 0;
	return dtypes[dt].elem_bits / 8;
}

size_t rk_dtype_block(enum rk_dtype dt)
{
	return (unsigned)dt < RK_DT_COUNT ? dtypes[dt].block : 1;
}

size_t rk_dtype_block_bytes(enum rk_dtype dt)
{
	return (unsigned)dt < RK_DT_COUNT ? dtypes[dt].block_bytes : 0;
}

const char *rk_dtype_name(enum rk_dtype dt)
{
	return (unsigned)dt < RK_DT_COUNT ? dtypes[dt].name : "?";
}

static size_t bytes_for(enum rk_dtype dt, u64 nelem)
{
	size_t block = rk_dtype_block(dt);
	size_t bb    = rk_dtype_block_bytes(dt);
	if (!block || !bb)
		return 0;
	return (size_t)DIV_ROUND_UP(nelem, block) * bb;
}

static void fill_strides(struct rk_tensor *t)
{
	/* Row-major: the last dimension is contiguous. */
	s64 acc = 1;
	for (int i = (int)t->ndim - 1; i >= 0; i--) {
		t->stride[i] = acc;
		acc *= (s64)t->shape[i];
	}
}

struct rk_tensor *rk_tensor_create(const char *name, enum rk_dtype dt,
                                   const u64 *shape, u8 ndim, u32 flags)
{
	if (!shape || !ndim || ndim > RK_TENSOR_MAX_DIMS || (unsigned)dt >= RK_DT_COUNT)
		return NULL;

	u64 nelem = 1;
	for (u8 i = 0; i < ndim; i++) {
		if (!shape[i])
			return NULL;
		/* Refuse an element count that would overflow the byte size computed
		 * below. A silently wrapped size here is a heap overflow later. */
		if (nelem > (1ull << 40) / shape[i])
			return NULL;
		nelem *= shape[i];
	}

	size_t nbytes = bytes_for(dt, nelem);
	if (!nbytes)
		return NULL;

	struct rk_tensor *t = kmem_cache_alloc(tensor_cache);
	if (!t)
		return NULL;
	memset(t, 0, sizeof(*t));

	t->id    = __atomic_add_fetch(&next_tensor_id, 1, __ATOMIC_SEQ_CST) - 1;
	t->dtype = (u8)dt;
	t->ndim  = ndim;
	t->flags = (u16)flags;
	t->nelem = nelem;
	t->nbytes = nbytes;
	t->refcount = 1;
	memcpy(t->shape, shape, ndim * sizeof(u64));
	fill_strides(t);
	strlcpy(t->name, name ? name : "tensor", sizeof(t->name));

	size_t npages = DIV_ROUND_UP(nbytes, RK_PAGE_SIZE);
	t->store = vm_object_anon(npages, "tensor");
	if (!t->store) {
		kmem_cache_free(tensor_cache, t);
		return NULL;
	}

	/* Kernel-side tensors are mapped into the direct map eagerly. A device
	 * tensor or a very large one would be demand-paged instead, but the ops
	 * here need a plain pointer. */
	t->data = kmalloc(nbytes);
	if (!t->data) {
		vm_object_put(t->store);
		kmem_cache_free(tensor_cache, t);
		return NULL;
	}
	memset(t->data, 0, nbytes);

	struct graph_node *n = rk_graph_node_create(GNODE_TENSOR, t->name, NULL, t);
	if (n) {
		rk_graph_set_str(n, "dtype", rk_dtype_name(dt));
		rk_graph_set_u64(n, "nelem", nelem);
		rk_graph_set_u64(n, "bytes", nbytes);
		t->graph_node = n->id;
	}
	return t;
}

/* A view shares the storage and only reinterprets the shape. The parent is
 * held for as long as the view lives, so the data cannot go away underneath. */
struct rk_tensor *rk_tensor_view(struct rk_tensor *src, const u64 *shape, u8 ndim,
                                 size_t elem_offset)
{
	if (!src || !shape || !ndim || ndim > RK_TENSOR_MAX_DIMS)
		return NULL;

	u64 nelem = 1;
	for (u8 i = 0; i < ndim; i++)
		nelem *= shape[i];
	if (elem_offset + nelem > src->nelem)
		return NULL;

	struct rk_tensor *t = kmem_cache_alloc(tensor_cache);
	if (!t)
		return NULL;
	memset(t, 0, sizeof(*t));

	t->id    = __atomic_add_fetch(&next_tensor_id, 1, __ATOMIC_SEQ_CST) - 1;
	t->dtype = src->dtype;
	t->ndim  = ndim;
	t->flags = src->flags;
	t->nelem = nelem;
	t->nbytes = bytes_for((enum rk_dtype)src->dtype, nelem);
	t->refcount = 1;
	t->view_of = src;
	t->store = src->store;
	memcpy(t->shape, shape, ndim * sizeof(u64));
	fill_strides(t);
	snprintf(t->name, sizeof(t->name), "%.32s.view", src->name);

	size_t byte_offset = bytes_for((enum rk_dtype)src->dtype, elem_offset);
	t->offset = src->offset + byte_offset;
	t->data = (u8 *)src->data + byte_offset;

	rk_tensor_get(src);
	return t;
}

/* Wrap memory the caller already owns, for weights mapped straight out of a
 * model file. No copy, and the tensor never frees it. */
struct rk_tensor *rk_tensor_wrap(const char *name, enum rk_dtype dt,
                                 const u64 *shape, u8 ndim, void *data, size_t nbytes)
{
	if (!data || !shape || !ndim || ndim > RK_TENSOR_MAX_DIMS)
		return NULL;

	struct rk_tensor *t = kmem_cache_alloc(tensor_cache);
	if (!t)
		return NULL;
	memset(t, 0, sizeof(*t));

	u64 nelem = 1;
	for (u8 i = 0; i < ndim; i++)
		nelem *= shape[i];
	if (bytes_for(dt, nelem) > nbytes) {
		kmem_cache_free(tensor_cache, t);
		return NULL;
	}

	t->id    = __atomic_add_fetch(&next_tensor_id, 1, __ATOMIC_SEQ_CST) - 1;
	t->dtype = (u8)dt;
	t->ndim  = ndim;
	t->nelem = nelem;
	t->nbytes = nbytes;
	t->refcount = 1;
	t->data = data;
	t->flags = RK_TENSOR_F_READONLY;
	memcpy(t->shape, shape, ndim * sizeof(u64));
	fill_strides(t);
	strlcpy(t->name, name ? name : "wrapped", sizeof(t->name));
	return t;
}

void rk_tensor_get(struct rk_tensor *t)
{
	if (t)
		__atomic_add_fetch(&t->refcount, 1, __ATOMIC_SEQ_CST);
}

void rk_tensor_put(struct rk_tensor *t)
{
	if (!t || __atomic_sub_fetch(&t->refcount, 1, __ATOMIC_SEQ_CST) != 0)
		return;

	if (t->view_of) {
		rk_tensor_put(t->view_of);
	} else {
		if (!(t->flags & RK_TENSOR_F_READONLY) && t->data)
			kfree(t->data);
		if (t->store)
			vm_object_put(t->store);
	}
	kmem_cache_free(tensor_cache, t);
}

int rk_tensor_copy(struct rk_tensor *dst, const struct rk_tensor *src)
{
	if (!dst || !src)
		return RK_EINVAL;
	if (dst->dtype != src->dtype || dst->nelem != src->nelem)
		return RK_EINVAL;
	if (dst->flags & RK_TENSOR_F_READONLY)
		return RK_EACCES;
	memcpy(dst->data, src->data, MIN(dst->nbytes, src->nbytes));
	return RK_OK;
}

size_t rk_tensor_describe(const struct rk_tensor *t, char *buf, size_t n)
{
	if (!t)
		return (size_t)snprintf(buf, n, "(null tensor)");

	size_t len = (size_t)snprintf(buf, n, "%s %s[", t->name, rk_dtype_name((enum rk_dtype)t->dtype));
	for (u8 i = 0; i < t->ndim; i++)
		len += (size_t)snprintf(buf + len, n > len ? n - len : 0, "%s%llu",
		                        i ? "," : "", (unsigned long long)t->shape[i]);
	len += (size_t)snprintf(buf + len, n > len ? n - len : 0, "] %pB%s",
	                        RK_BYTES(t->nbytes),
	                        t->view_of ? " (view)" : "");
	return len;
}

void rk_tensor_init(void)
{
	tensor_cache = kmem_cache_create("tensor", sizeof(struct rk_tensor), 64);
}
