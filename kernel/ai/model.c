/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - model registry and loader.
 *
 * Two things a userspace runtime cannot do, and the reasons this lives here:
 *
 *   Weights are loaded once for the machine, not once per process. Two tasks
 *   using the same model share the tensors through the capability system, so a
 *   second session costs its KV cache and nothing else.
 *
 *   A model has provenance. The digest covers every weight byte in canonical
 *   tensor order, and a Kaalka seal binds that digest to a time and an
 *   authority. A model whose bytes changed after it was sealed cannot be bound
 *   to an inference capability, so "which model actually ran" is answerable.
 *
 * GGUF is parsed because it is what quantised models are actually distributed
 * as. The parser reads the header and the tensor table and maps the data in
 * place; it does not copy weights.
 */
#include <rk/ai.h>
#include <rk/mm.h>
#include <rk/vfs.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/crypto.h>
#include <rk/graph.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "model"

static LIST_HEAD(models);
static struct mutex model_lock;
static rk_id_t next_model_id = 1;

/* ------------------------------------------------------------------ GGUF */

#define GGUF_MAGIC 0x46554747u   /* "GGUF" little endian */

enum gguf_type {
	GGUF_U8 = 0, GGUF_I8, GGUF_U16, GGUF_I16, GGUF_U32, GGUF_I32,
	GGUF_F32, GGUF_BOOL, GGUF_STRING, GGUF_ARRAY, GGUF_U64, GGUF_I64,
	GGUF_F64
};

/* GGUF ggml type ids that map onto our dtypes. Anything else is refused with
 * a clear message rather than silently misread. */
static enum rk_dtype gguf_dtype(u32 ggml_type)
{
	switch (ggml_type) {
	case 0:  return RK_DT_F32;
	case 1:  return RK_DT_F16;
	case 2:  return RK_DT_Q4_0;
	case 8:  return RK_DT_Q8_0;
	case 12: return RK_DT_Q4_K;
	case 24: return RK_DT_I8;
	case 25: return RK_DT_I16;
	case 26: return RK_DT_I32;
	case 30: return RK_DT_BF16;
	default: return RK_DT_INVALID;
	}
}

struct cursor {
	const u8 *p;
	const u8 *end;
	bool      bad;
};

static u64 rd(struct cursor *c, size_t n)
{
	if (c->p + n > c->end) {
		c->bad = true;
		return 0;
	}
	u64 v = 0;
	for (size_t i = 0; i < n; i++)
		v |= (u64)c->p[i] << (i * 8);
	c->p += n;
	return v;
}

/* GGUF strings are a u64 length then raw bytes. Copied into a bounded buffer
 * because a length field from a file is not a length we trust. */
static void rd_str(struct cursor *c, char *out, size_t cap)
{
	u64 len = rd(c, 8);
	if (c->bad || c->p + len > c->end || len > (1u << 20)) {
		c->bad = true;
		if (cap)
			out[0] = '\0';
		return;
	}
	size_t take = len < cap - 1 ? (size_t)len : cap - 1;
	memcpy(out, c->p, take);
	out[take] = '\0';
	c->p += len;
}

static void skip_value(struct cursor *c, u32 type);

static void skip_array(struct cursor *c)
{
	u32 elem = (u32)rd(c, 4);
	u64 n = rd(c, 8);
	for (u64 i = 0; i < n && !c->bad; i++)
		skip_value(c, elem);
}

static void skip_value(struct cursor *c, u32 type)
{
	switch (type) {
	case GGUF_U8: case GGUF_I8: case GGUF_BOOL: rd(c, 1); break;
	case GGUF_U16: case GGUF_I16:               rd(c, 2); break;
	case GGUF_U32: case GGUF_I32: case GGUF_F32: rd(c, 4); break;
	case GGUF_U64: case GGUF_I64: case GGUF_F64: rd(c, 8); break;
	case GGUF_STRING: { char tmp[8]; rd_str(c, tmp, sizeof(tmp)); break; }
	case GGUF_ARRAY: skip_array(c); break;
	default: c->bad = true; break;
	}
}

/* Architecture keys we care about. Anything else is skipped: a model that
 * declares extra metadata should still load. */
static void apply_meta(struct rk_model *m, const char *key, u32 type,
                       struct cursor *c)
{
	u64 v = 0;
	bool numeric = true;

	switch (type) {
	case GGUF_U8: case GGUF_I8: case GGUF_BOOL: v = rd(c, 1); break;
	case GGUF_U16: case GGUF_I16:               v = rd(c, 2); break;
	case GGUF_U32: case GGUF_I32:               v = rd(c, 4); break;
	case GGUF_U64: case GGUF_I64:               v = rd(c, 8); break;
	case GGUF_F32: {
		u32 bits = (u32)rd(c, 4);
		float f;
		memcpy(&f, &bits, sizeof(f));
		if (strstr(key, "rope.freq_base"))
			m->rope_theta = f;
		else if (strstr(key, "layer_norm") || strstr(key, "rms_norm"))
			m->norm_eps = f;
		return;
	}
	default:
		numeric = false;
		skip_value(c, type);
		break;
	}
	if (!numeric)
		return;

	if (strstr(key, "block_count"))            m->n_layers   = (u32)v;
	else if (strstr(key, "attention.head_count_kv")) m->n_kv_heads = (u32)v;
	else if (strstr(key, "attention.head_count"))    m->n_heads    = (u32)v;
	else if (strstr(key, "embedding_length"))  m->embed_dim  = (u32)v;
	else if (strstr(key, "feed_forward_length")) m->ffn_dim  = (u32)v;
	else if (strstr(key, "context_length"))    m->context_len = (u32)v;
	else if (strstr(key, "vocab_size"))        m->vocab_size = (u32)v;
}

static int gguf_parse(struct rk_model *m, const u8 *base, size_t len)
{
	struct cursor c = { base, base + len, false };

	if (rd(&c, 4) != GGUF_MAGIC)
		return RK_EINVAL;
	u32 version = (u32)rd(&c, 4);
	if (version < 2 || version > 3) {
		pr_err("GGUF version %u not supported (need 2 or 3)", version);
		return RK_ENOTSUP;
	}

	u64 ntensors = rd(&c, 8);
	u64 nkv      = rd(&c, 8);
	if (c.bad || ntensors > RK_MODEL_MAX_TENSORS) {
		pr_err("GGUF: %llu tensors exceeds the %u the kernel will map",
		       (unsigned long long)ntensors, RK_MODEL_MAX_TENSORS);
		return RK_E2BIG;
	}

	for (u64 i = 0; i < nkv && !c.bad; i++) {
		char key[128];
		rd_str(&c, key, sizeof(key));
		u32 type = (u32)rd(&c, 4);
		apply_meta(m, key, type, &c);
	}
	if (c.bad)
		return RK_EINVAL;

	/* Tensor table: name, dimension count, dimensions, type, offset. */
	struct tinfo {
		char name[64];
		u64  shape[RK_TENSOR_MAX_DIMS];
		u8   ndim;
		u32  ggml_type;
		u64  offset;
	};
	struct tinfo *infos = kcalloc((size_t)ntensors, sizeof(struct tinfo));
	if (!infos)
		return RK_ENOMEM;

	for (u64 i = 0; i < ntensors && !c.bad; i++) {
		rd_str(&c, infos[i].name, sizeof(infos[i].name));
		u32 nd = (u32)rd(&c, 4);
		if (nd > RK_TENSOR_MAX_DIMS) {
			c.bad = true;
			break;
		}
		infos[i].ndim = (u8)nd;
		/* GGUF lists dimensions fastest-varying first; our tensors are
		 * row-major, so reverse them here rather than everywhere else. */
		for (u32 d = 0; d < nd; d++)
			infos[i].shape[nd - 1 - d] = rd(&c, 8);
		infos[i].ggml_type = (u32)rd(&c, 4);
		infos[i].offset    = rd(&c, 8);
	}
	if (c.bad) {
		kfree(infos);
		return RK_EINVAL;
	}

	/* Data begins at the next alignment boundary after the header. */
	u64 align = 32;
	u64 data_start = (u64)ALIGN_UP((u64)(c.p - base), align);

	m->tensors = kcalloc((size_t)ntensors, sizeof(struct rk_tensor *));
	if (!m->tensors) {
		kfree(infos);
		return RK_ENOMEM;
	}

	u32 mapped = 0;
	for (u64 i = 0; i < ntensors; i++) {
		enum rk_dtype dt = gguf_dtype(infos[i].ggml_type);
		if (dt == RK_DT_INVALID) {
			pr_warn("tensor %s uses ggml type %u, skipping",
			        infos[i].name, infos[i].ggml_type);
			continue;
		}
		u64 nelem = 1;
		for (u8 d = 0; d < infos[i].ndim; d++)
			nelem *= infos[i].shape[d];

		size_t block = rk_dtype_block(dt);
		size_t nbytes = (size_t)DIV_ROUND_UP(nelem, block) * rk_dtype_block_bytes(dt);
		u64 off = data_start + infos[i].offset;
		if (off + nbytes > len) {
			pr_err("tensor %s runs past the end of the file", infos[i].name);
			continue;
		}

		struct rk_tensor *t = rk_tensor_wrap(infos[i].name, dt, infos[i].shape,
		                                     infos[i].ndim, (void *)(base + off),
		                                     nbytes);
		if (t) {
			m->tensors[mapped++] = t;
			m->bytes_resident += nbytes;
		}
	}
	m->ntensors = mapped;
	m->format = RK_MODEL_GGUF;

	if (!m->head_dim && m->n_heads && m->embed_dim)
		m->head_dim = m->embed_dim / m->n_heads;
	if (!m->n_kv_heads)
		m->n_kv_heads = m->n_heads;
	if (m->rope_theta == 0.0f)
		m->rope_theta = 10000.0f;
	if (m->norm_eps == 0.0f)
		m->norm_eps = 1e-5f;

	kfree(infos);
	return RK_OK;
}

/* ------------------------------------------------------------- provenance */

/* Digest over every weight byte in canonical tensor order. Tensor names are
 * included, so renaming a tensor changes the digest: a model is its weights
 * and the structure they sit in, not just the bytes. */
static void model_digest(struct rk_model *m)
{
	struct sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, "RESENTMENT/model/v1", 19);

	for (u32 i = 0; i < m->ntensors; i++) {
		struct rk_tensor *t = m->tensors[i];
		if (!t)
			continue;
		sha256_update(&c, t->name, strnlen(t->name, sizeof(t->name)));
		sha256_update(&c, &t->dtype, sizeof(t->dtype));
		sha256_update(&c, t->shape, sizeof(u64) * t->ndim);
		sha256_update(&c, t->data, t->nbytes);
	}
	sha256_final(&c, m->digest);
}

int rk_model_verify(struct rk_model *m)
{
	if (!m)
		return RK_EINVAL;

	u8 want[SHA256_DIGEST_SIZE];
	memcpy(want, m->digest, sizeof(want));
	model_digest(m);

	if (!rk_ct_eq(want, m->digest, sizeof(want))) {
		m->verified = false;
		pr_err("model %s failed digest verification: weights changed since sealing",
		       m->name);
		return RK_ESEAL;
	}

	int r = kaalka_seal_verify(&m->seal, m->id, m->digest, sizeof(m->digest));
	m->verified = (r == RK_OK);
	if (!m->verified)
		pr_err("model %s seal invalid: %s", m->name, rk_strerror(r));
	return r;
}

/* --------------------------------------------------------------- registry */

int rk_model_register(struct rk_model *m)
{
	if (!m)
		return RK_EINVAL;

	m->id = __atomic_add_fetch(&next_model_id, 1, __ATOMIC_SEQ_CST) - 1;
	m->refcount = 1;
	mutex_init(&m->lock, "model");
	list_init(&m->link);

	model_digest(m);
	/* Sealed for a day. Long enough that a model stays usable across a work
	 * session, short enough that a stale one is noticed. */
	kaalka_seal_make(&m->seal, m->id, 0, m->digest, sizeof(m->digest), 86400);
	m->verified = true;

	mutex_lock(&model_lock);
	list_add_tail(&m->link, &models);
	mutex_unlock(&model_lock);

	char hex[17];
	rk_hex_encode(hex, sizeof(hex), m->digest, 8);

	struct graph_node *n = rk_graph_node_create(GNODE_MODEL, m->name, NULL, m);
	if (n) {
		rk_graph_set_u64(n, "tensors", m->ntensors);
		rk_graph_set_u64(n, "bytes", m->bytes_resident);
		rk_graph_set_u64(n, "layers", m->n_layers);
		rk_graph_set_str(n, "digest", hex);
		m->graph_node = n->id;
	}

	pr_info("model %s: %u tensors, %pB, %u layers, digest %s...",
	        m->name, m->ntensors, RK_BYTES(m->bytes_resident), m->n_layers, hex);
	return RK_OK;
}

struct rk_model *rk_model_load(const char *path, const char *name)
{
	void *buf = NULL;
	size_t len = 0;

	int r = rk_vfs_read_file(path, &buf, &len);
	if (r != RK_OK) {
		pr_err("cannot read %s: %s", path, rk_strerror(r));
		return NULL;
	}

	struct rk_model *m = kzalloc(sizeof(*m));
	if (!m) {
		kfree(buf);
		return NULL;
	}
	strlcpy(m->name, name ? name : path, sizeof(m->name));

	r = gguf_parse(m, buf, len);
	if (r != RK_OK) {
		pr_err("%s is not a model this kernel can load: %s", path, rk_strerror(r));
		kfree(m->tensors);
		kfree(m);
		kfree(buf);
		return NULL;
	}

	/* The buffer stays alive: the tensors point into it. */
	if (rk_model_register(m) != RK_OK) {
		kfree(m->tensors);
		kfree(m);
		kfree(buf);
		return NULL;
	}
	return m;
}

struct rk_model *rk_model_find(const char *name)
{
	struct rk_model *m;
	list_for_each_entry(m, &models, link)
		if (strcmp(m->name, name) == 0)
			return m;
	return NULL;
}

void rk_model_get(struct rk_model *m)
{
	if (m)
		__atomic_add_fetch(&m->refcount, 1, __ATOMIC_SEQ_CST);
}

void rk_model_put(struct rk_model *m)
{
	if (!m || __atomic_sub_fetch(&m->refcount, 1, __ATOMIC_SEQ_CST) != 0)
		return;

	mutex_lock(&model_lock);
	list_del(&m->link);
	mutex_unlock(&model_lock);

	for (u32 i = 0; i < m->ntensors; i++)
		rk_tensor_put(m->tensors[i]);
	kfree(m->tensors);
	kfree(m);
}

struct rk_tensor *rk_model_tensor(struct rk_model *m, const char *name)
{
	if (!m)
		return NULL;
	for (u32 i = 0; i < m->ntensors; i++)
		if (m->tensors[i] && strcmp(m->tensors[i]->name, name) == 0)
			return m->tensors[i];
	return NULL;
}

size_t rk_model_list(struct rk_model **out, size_t max)
{
	size_t n = 0;
	struct rk_model *m;
	list_for_each_entry(m, &models, link) {
		if (n >= max)
			break;
		out[n++] = m;
	}
	return n;
}

void rk_model_init(void)
{
	mutex_init(&model_lock, "model-registry");
}
