/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the AI subsystem.
 *
 * The premise of RESENTMENT is that inference is not an application, it is a
 * system resource, and the kernel is the only place that can arbitrate it
 * honestly. A userspace runtime cannot see that two processes are thrashing
 * the same weights, cannot preempt a half-finished decode to service a
 * latency-critical one, and cannot stop a model it did not load from reading
 * memory it was never granted.
 *
 * So four things live here that normally live in a library:
 *
 *   tensors        first-class kernel objects backed by vm_objects, so a
 *                  tensor can be shared between tasks with zero copies and
 *                  revoked with a capability
 *   model registry weights are loaded once, refcounted, and content-addressed
 *                  into the runtime graph with a Kaalka-sealed digest, so a
 *                  model that was tampered with will not run
 *   paged KV cache attention caches are paged like memory, with eviction and
 *                  sharing of common prefixes, instead of each process
 *                  reserving a worst-case slab it mostly wastes
 *   accel HAL      one interface over CPU SIMD, GPU, NPU and TPU queues, so a
 *                  phone NPU and a server GPU are the same object to a caller
 *
 * Scheduling of the actual work is SCHED_INFERENCE in rk/sched.h: token
 * generation is a soft-real-time stream with a target rate, and treating it
 * as batch is exactly why inference stutters on general purpose systems.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/sync.h>
#include <rk/crypto.h>
#include <rk/kaalka.h>

/* ---------------------------------------------------------------- tensors */

enum rk_dtype {
	RK_DT_INVALID = 0,
	RK_DT_F32, RK_DT_F16, RK_DT_BF16,
	RK_DT_I32, RK_DT_I16, RK_DT_I8, RK_DT_U8,
	RK_DT_Q8_0,    /* 8-bit block-quantised, 32 values per block + f16 scale */
	RK_DT_Q4_0,    /* 4-bit block-quantised, 32 values per block + f16 scale */
	RK_DT_Q4_K,    /* 4-bit, per-superblock scales */
	RK_DT_BOOL,
	RK_DT_COUNT
};

#define RK_TENSOR_MAX_DIMS 6
#define RK_TENSOR_NAME_MAX 48

struct vm_object;

struct rk_tensor {
	rk_id_t           id;
	char              name[RK_TENSOR_NAME_MAX];
	u8                dtype;
	u8                ndim;
	u16               flags;
	u64               shape[RK_TENSOR_MAX_DIMS];
	s64               stride[RK_TENSOR_MAX_DIMS];  /* in elements, may be 0 for broadcast */
	u64               nelem;
	size_t            nbytes;

	struct vm_object *store;      /* backing pages */
	size_t            offset;     /* byte offset into the store */
	void             *data;       /* kernel virtual pointer, mapped on demand */

	u32               refcount;
	rk_id_t           graph_node;
	struct rk_tensor *view_of;    /* non-NULL for zero-copy views */
};

#define RK_TENSOR_F_READONLY  (1u << 0)
#define RK_TENSOR_F_PINNED    (1u << 1)   /* never paged out; required for DMA */
#define RK_TENSOR_F_DEVICE    (1u << 2)   /* lives in accelerator memory */
#define RK_TENSOR_F_QUANT     (1u << 3)
#define RK_TENSOR_F_SHARED    (1u << 4)

size_t rk_dtype_size(enum rk_dtype dt);      /* bytes per element, 0 if blocked */
size_t rk_dtype_block(enum rk_dtype dt);     /* elements per block, 1 if dense */
size_t rk_dtype_block_bytes(enum rk_dtype dt);
const char *rk_dtype_name(enum rk_dtype dt);

struct rk_tensor *rk_tensor_create(const char *name, enum rk_dtype dt,
                                   const u64 *shape, u8 ndim, u32 flags);
struct rk_tensor *rk_tensor_view(struct rk_tensor *src, const u64 *shape, u8 ndim,
                                 size_t elem_offset);
struct rk_tensor *rk_tensor_wrap(const char *name, enum rk_dtype dt,
                                 const u64 *shape, u8 ndim, void *data, size_t nbytes);
void  rk_tensor_get(struct rk_tensor *t);
void  rk_tensor_put(struct rk_tensor *t);
int   rk_tensor_fill(struct rk_tensor *t, float value);
int   rk_tensor_copy(struct rk_tensor *dst, const struct rk_tensor *src);
size_t rk_tensor_describe(const struct rk_tensor *t, char *buf, size_t n);

/* Typed element access, so callers outside this directory do not have to know
 * how each dtype is laid out. Both convert through f32. */
float rk_tensor_get_f32(const struct rk_tensor *t, u64 index);
void  rk_tensor_set_f32(struct rk_tensor *t, u64 index, float v);
float rk_f16_to_f32(u16 h);
u16   rk_f32_to_f16(float f);

void  rk_tensor_init(void);
void  rk_accel_init(void);
void  rk_model_init(void);
void  rk_kv_init(u64 budget_bytes);

/* ----------------------------------------------------------------- ops */

enum rk_op {
	RK_OP_ADD = 0, RK_OP_SUB, RK_OP_MUL, RK_OP_DIV,
	RK_OP_MATMUL, RK_OP_MATVEC,
	RK_OP_RELU, RK_OP_GELU, RK_OP_SILU, RK_OP_TANH, RK_OP_SIGMOID,
	RK_OP_SOFTMAX, RK_OP_LAYERNORM, RK_OP_RMSNORM,
	RK_OP_ROPE, RK_OP_ATTENTION,
	RK_OP_EMBED, RK_OP_ARGMAX, RK_OP_TOPK,
	RK_OP_QUANTIZE, RK_OP_DEQUANTIZE,
	RK_OP_TRANSPOSE, RK_OP_CONCAT, RK_OP_SLICE,
	RK_OP_CONV2D, RK_OP_MAXPOOL2D,
	RK_OP_COUNT
};

struct rk_op_args {
	float  scale;
	float  eps;
	s32    axis;
	s32    k;
	u32    heads;
	u32    head_dim;
	u64    pos;        /* token position, for RoPE */
	u32    flags;
};

const char *rk_op_name(enum rk_op op);

/* Execute one op. dst may alias a src for in-place ops that allow it. */
int rk_op_exec(enum rk_op op, struct rk_tensor *dst,
               struct rk_tensor *a, struct rk_tensor *b, struct rk_tensor *c,
               const struct rk_op_args *args);

/* ------------------------------------------------------- accelerator HAL */

enum rk_accel_kind { RK_ACCEL_CPU = 0, RK_ACCEL_GPU, RK_ACCEL_NPU, RK_ACCEL_TPU,
                     RK_ACCEL_DSP, RK_ACCEL_FPGA, RK_ACCEL_REMOTE };

struct rk_accel;

struct rk_accel_ops {
	int  (*submit)(struct rk_accel *a, enum rk_op op, struct rk_tensor *dst,
	               struct rk_tensor *x, struct rk_tensor *y, struct rk_tensor *z,
	               const struct rk_op_args *args, u64 *fence);
	int  (*wait)(struct rk_accel *a, u64 fence, u64 timeout_ns);
	int  (*alloc)(struct rk_accel *a, size_t bytes, void **out, u64 *dev_addr);
	void (*free)(struct rk_accel *a, void *p);
	int  (*upload)(struct rk_accel *a, void *dev, const void *host, size_t n);
	int  (*download)(struct rk_accel *a, void *host, const void *dev, size_t n);
	bool (*supports)(struct rk_accel *a, enum rk_op op, enum rk_dtype dt);
};

struct rk_accel {
	rk_id_t                     id;
	char                        name[32];
	enum rk_accel_kind          kind;
	const struct rk_accel_ops  *ops;
	void                       *priv;
	u64                         mem_total, mem_free;
	u64                         peak_ops_per_sec;   /* declared, for scheduling */
	u32                         queue_depth;
	u64                         submitted, completed, errors;
	u64                         busy_ns;
	struct list_head            link;
	rk_id_t                     graph_node;
	struct mutex                lock;
};

int  rk_accel_register(struct rk_accel *a);
void rk_accel_unregister(struct rk_accel *a);
struct rk_accel *rk_accel_find(const char *name);
struct rk_accel *rk_accel_best(enum rk_op op, enum rk_dtype dt);
size_t rk_accel_list(struct rk_accel **out, size_t max);

/* ------------------------------------------------------------ model registry */

#define RK_MODEL_NAME_MAX 64
#define RK_MODEL_MAX_TENSORS 1024

enum rk_model_format { RK_MODEL_RAW = 0, RK_MODEL_GGUF, RK_MODEL_SAFETENSORS, RK_MODEL_ONNX };

struct rk_model {
	rk_id_t            id;
	char               name[RK_MODEL_NAME_MAX];
	u8                 format;
	u32                flags;

	u32                ntensors;
	struct rk_tensor **tensors;

	/* Architecture description, kept generic so this is not a transformer-only
	 * kernel: a vision or audio model fills in what applies and ignores rest. */
	u32                n_layers, n_heads, n_kv_heads, embed_dim, ffn_dim;
	u32                vocab_size, context_len, head_dim;
	float              rope_theta, norm_eps;

	/* Provenance. The digest covers every weight byte in canonical tensor
	 * order; the seal binds that digest to a time and an authority. A model
	 * that fails verification cannot be bound to an inference capability. */
	u8                 digest[SHA256_DIGEST_SIZE];
	struct kaalka_seal seal;
	bool               verified;

	u64                bytes_resident;
	u32                refcount;
	rk_id_t            graph_node;
	struct list_head   link;
	struct mutex       lock;
};

int  rk_model_register(struct rk_model *m);
struct rk_model *rk_model_load(const char *path, const char *name);
struct rk_model *rk_model_find(const char *name);
void rk_model_get(struct rk_model *m);
void rk_model_put(struct rk_model *m);
int  rk_model_verify(struct rk_model *m);
struct rk_tensor *rk_model_tensor(struct rk_model *m, const char *name);
size_t rk_model_list(struct rk_model **out, size_t max);

/* --------------------------------------------------------- paged KV cache */

/* Attention caches are paged exactly like memory. Pages are shared between
 * sequences that share a prefix (a system prompt is stored once for every
 * session that uses it) and evicted under pressure rather than pre-reserved.
 */
#define RK_KV_PAGE_TOKENS 16

struct rk_kv_page {
	struct list_head hash;    /* content-address bucket */
	struct list_head lru;     /* eviction order */
	rk_id_t          id;
	u8               digest[SHA256_DIGEST_SIZE];  /* of the token prefix it holds */
	u32              refcount;
	u32              bytes;   /* k plus v, so eviction can account it */
	u16              ntokens;
	u16              layer;
	void            *k, *v;
	u64              last_used_ns;
};

struct rk_kv_cache {
	rk_id_t           id;
	rk_id_t           seq_id;
	u32               n_layers, n_kv_heads, head_dim;
	u8                dtype;
	u32               npages, max_pages;
	struct rk_kv_page **pages;
	u64               tokens;
	struct mutex      lock;
	rk_id_t           graph_node;
};

struct rk_kv_cache *rk_kv_create(rk_id_t seq_id, u32 layers, u32 kv_heads,
                                 u32 head_dim, enum rk_dtype dt, u32 max_tokens);
void rk_kv_destroy(struct rk_kv_cache *c);
int  rk_kv_append(struct rk_kv_cache *c, u32 layer, const void *k, const void *v, u32 ntok);
int  rk_kv_fork(struct rk_kv_cache *dst, struct rk_kv_cache *src, u32 upto_token);
void rk_kv_trim(struct rk_kv_cache *c, u32 keep_tokens);
void rk_kv_pressure_evict(u64 want_bytes);
void rk_kv_stats(u64 *pages, u64 *shared, u64 *bytes, u64 *evictions);

/* --------------------------------------------------- inference scheduling */

enum rk_infer_state { RK_INFER_QUEUED = 0, RK_INFER_RUNNING, RK_INFER_DONE,
                      RK_INFER_FAILED, RK_INFER_CANCELED, RK_INFER_PREEMPTED };

struct rk_infer_req {
	rk_id_t              id;
	struct rk_model     *model;
	struct rk_kv_cache  *kv;
	struct rk_tensor    *input;
	struct rk_tensor    *output;

	u64                  deadline_ns;    /* 0 = best effort */
	u64                  target_rate;    /* tokens/sec the caller wants */
	u32                  max_tokens;
	u32                  produced;
	u32                  priority;
	bool                 preemptible;

	volatile u32         state;
	int                  error;
	struct completion    done;
	struct list_head     link;
	rk_id_t              owner_task;
	rk_id_t              graph_node;
	u64                  queued_ns, started_ns, finished_ns;
};

void rk_infer_init(void);
int  rk_infer_submit(struct rk_infer_req *r);
int  rk_infer_wait(struct rk_infer_req *r, u64 timeout_ns);
int  rk_infer_cancel(struct rk_infer_req *r);

struct rk_infer_stats {
	u64 submitted, completed, failed, canceled, preempted;
	u64 tokens_generated;
	u64 queue_depth, deadline_misses;
	u64 total_compute_ns, total_wait_ns;
};
void rk_infer_stats(struct rk_infer_stats *out);

/* ----------------------------------------------------- kernel-side agency */

/* A small, bounded hook so the kernel can *ask* rather than only *decide*.
 * A registered advisor is consulted for policy questions that have no correct
 * static answer: which page to evict, whether to admit a real-time task, how
 * to place a thread across cores. Advisors are strictly advisory, run with a
 * hard deadline, and their answers are clamped to a safe range; if one is slow
 * or absent the kernel falls back to its deterministic heuristic. An operating
 * system may consult a model, but it may never depend on one. */
enum rk_advice_topic {
	RK_ADVICE_PAGE_EVICT = 0,
	RK_ADVICE_SCHED_PLACE,
	RK_ADVICE_RT_ADMIT,
	RK_ADVICE_PREFETCH,
	RK_ADVICE_KV_EVICT,
	RK_ADVICE_POWER,
	RK_ADVICE_COUNT
};

struct rk_advice_query {
	enum rk_advice_topic topic;
	u64 subject;
	u64 candidates[8];
	u32 ncandidates;
	u64 hint[4];
};

typedef int (*rk_advisor_fn)(const struct rk_advice_query *q, u64 *choice, void *ctx);

int  rk_advisor_register(enum rk_advice_topic topic, rk_advisor_fn fn, void *ctx,
                         u64 deadline_ns);
void rk_advisor_unregister(enum rk_advice_topic topic);
/* Returns the advisor choice, or fallback when there is none or it was late. */
u64  rk_advice_ask(const struct rk_advice_query *q, u64 fallback);
void rk_advisor_stats(enum rk_advice_topic t, u64 *asked, u64 *answered,
                      u64 *late, u64 *rejected);

void rk_ai_init(void);
int  rk_ai_selftest(void);
