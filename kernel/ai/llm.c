/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - a transformer forward pass, in the kernel.
 *
 * This exists to close a specific gap. The kernel had tensors, twenty
 * operators, an accelerator layer, a GGUF loader and a scheduling class named
 * after inference - and nothing that actually ran a model. The inference
 * "step" was a single matrix-vector product standing in for a forward pass,
 * which meant the one claim the whole design rests on was the one claim that
 * had never been executed.
 *
 * So: embed, then for every layer normalise, project to queries keys and
 * values, rotate, attend over the history, project back, add; normalise,
 * gate, add; and at the end normalise and project to logits. Every arithmetic
 * step goes through rk_op_exec, so the accounting, the accelerator dispatch
 * and the graph events cover real work rather than a placeholder.
 *
 * Two things this is not:
 *
 * It is not a claim about model quality. The fixture it runs has pseudo-random
 * weights and the tokens it produces mean nothing. The claim is that the path
 * exists and is scheduled correctly, which is a systems claim and is the only
 * kind a kernel is entitled to make.
 *
 * It is not the paged KV cache. This keeps its own contiguous history because
 * attention needs a head's keys laid out consecutively and the paged cache
 * stores sixteen-token blocks. Putting the decode loop onto rk_kv is the next
 * step and is written up in docs/AI.md rather than implied here.
 */
#include <rk/ai.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/log.h>
#include <rk/printf.h>
#include <rk/time.h>
#include <rk/sched.h>
#include <rk/graph.h>
#include <rk/arch.h>
#include <rk/console.h>

#undef RK_SUBSYS
#define RK_SUBSYS "llm"

/* Bounded because this runs in the kernel: a model whose header claims
 * unreasonable dimensions must be refused rather than allocated for. */
#define LLM_MAX_LAYERS  32
#define LLM_MAX_EMBED   4096
#define LLM_MAX_CONTEXT 2048

struct rk_llm {
	struct rk_model *model;

	u32 n_layers, embed, heads, kv_heads, head_dim, ffn, vocab, context;
	float eps, theta;

	/* Weights, resolved once. A tensor missing here is a model this kernel
	 * cannot run, and finding that out at open time beats finding it out
	 * halfway through a token. */
	struct rk_tensor *tok_embd, *out_norm, *out_w;
	struct {
		struct rk_tensor *attn_norm, *wq, *wk, *wv, *wo;
		struct rk_tensor *ffn_norm, *gate, *up, *down;
	} layer[LLM_MAX_LAYERS];

	/* Activations, allocated once and reused for every token. */
	struct rk_tensor *x, *h, *q, *k, *v, *attn, *g, *u, *logits, *idv;

	/* Views onto one head's slice of the above, and onto its history. These
	 * are created once and repointed per head rather than allocated inside
	 * the loop: a forward pass runs with preemption disabled, because the
	 * float unit is in use, and an allocator call in that window is a way to
	 * deadlock rather than an optimisation worth arguing about. */
	struct rk_tensor *qv, *kv, *vv, *ov;

	/* History, laid out [layer][head][position][head_dim] so that one head's
	 * keys are consecutive - which is what the attention operator wants and
	 * the only reason this is not simply [layer][position][embed]. */
	float *kc, *vc;
	u32    pos;

	u64 tokens, compute_ns;
};

/* --------------------------------------------------------------- helpers */

static struct rk_tensor *want(struct rk_model *m, const char *fmt, ...)
{
	char name[RK_TENSOR_NAME_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(name, sizeof(name), fmt, ap);
	va_end(ap);

	struct rk_tensor *t = rk_model_tensor(m, name);
	if (!t)
		pr_warn("model %s has no tensor %s", m->name, name);
	return t;
}

static struct rk_tensor *scratch(const char *name, u64 n)
{
	u64 shape[1] = { n };
	return rk_tensor_create(name, RK_DT_F32, shape, 1, 0);
}

static void add_into(struct rk_tensor *dst, struct rk_tensor *src)
{
	for (u64 i = 0; i < dst->nelem && i < src->nelem; i++)
		rk_tensor_set_f32(dst, i, rk_tensor_get_f32(dst, i) +
		                          rk_tensor_get_f32(src, i));
}

/* ------------------------------------------------------------------ open */

struct rk_llm *rk_llm_open(struct rk_model *m, u32 context)
{
	if (!m)
		return NULL;

	if (!m->n_layers || m->n_layers > LLM_MAX_LAYERS ||
	    !m->embed_dim || m->embed_dim > LLM_MAX_EMBED ||
	    !m->n_heads || !m->head_dim || !m->vocab_size) {
		pr_err("model %s does not describe a transformer this kernel can run",
		       m->name);
		return NULL;
	}
	if (m->head_dim * m->n_heads != m->embed_dim) {
		pr_err("model %s: %u heads of %u do not make an embedding of %u",
		       m->name, m->n_heads, m->head_dim, m->embed_dim);
		return NULL;
	}

	if (!context)
		context = m->context_len ? m->context_len : 64;
	if (context > LLM_MAX_CONTEXT)
		context = LLM_MAX_CONTEXT;

	struct rk_llm *c = kzalloc(sizeof(*c));
	if (!c)
		return NULL;

	c->model   = m;
	c->n_layers = m->n_layers;
	c->embed   = m->embed_dim;
	c->heads   = m->n_heads;
	c->kv_heads = m->n_kv_heads ? m->n_kv_heads : m->n_heads;
	c->head_dim = m->head_dim;
	c->ffn     = m->ffn_dim ? m->ffn_dim : m->embed_dim * 2;
	c->vocab   = m->vocab_size;
	c->context = context;
	c->eps     = m->norm_eps;
	c->theta   = m->rope_theta;

	c->tok_embd = want(m, "token_embd.weight");
	c->out_norm = want(m, "output_norm.weight");
	c->out_w    = want(m, "output.weight");
	if (!c->tok_embd || !c->out_norm || !c->out_w)
		goto fail;

	for (u32 i = 0; i < c->n_layers; i++) {
		c->layer[i].attn_norm = want(m, "blk.%u.attn_norm.weight", i);
		c->layer[i].wq        = want(m, "blk.%u.attn_q.weight", i);
		c->layer[i].wk        = want(m, "blk.%u.attn_k.weight", i);
		c->layer[i].wv        = want(m, "blk.%u.attn_v.weight", i);
		c->layer[i].wo        = want(m, "blk.%u.attn_output.weight", i);
		c->layer[i].ffn_norm  = want(m, "blk.%u.ffn_norm.weight", i);
		c->layer[i].gate      = want(m, "blk.%u.ffn_gate.weight", i);
		c->layer[i].up        = want(m, "blk.%u.ffn_up.weight", i);
		c->layer[i].down      = want(m, "blk.%u.ffn_down.weight", i);
		if (!c->layer[i].attn_norm || !c->layer[i].wq || !c->layer[i].wk ||
		    !c->layer[i].wv || !c->layer[i].wo || !c->layer[i].ffn_norm ||
		    !c->layer[i].gate || !c->layer[i].up || !c->layer[i].down)
			goto fail;
	}

	c->x      = scratch("x", c->embed);
	c->h      = scratch("h", c->embed);
	c->q      = scratch("q", c->embed);
	c->k      = scratch("k", c->embed);
	c->v      = scratch("v", c->embed);
	c->attn   = scratch("attn", c->embed);
	c->g      = scratch("gate", c->ffn);
	c->u      = scratch("up", c->ffn);
	c->logits = scratch("logits", c->vocab);
	c->idv    = scratch("id", 1);
	if (!c->x || !c->h || !c->q || !c->k || !c->v || !c->attn ||
	    !c->g || !c->u || !c->logits || !c->idv)
		goto fail;

	u64 hd[1] = { c->head_dim };
	u64 full[1] = { (u64)c->context * c->head_dim };
	c->qv = rk_tensor_wrap("q.head", RK_DT_F32, hd, 1, c->q->data,
	                       c->head_dim * sizeof(float));
	c->ov = rk_tensor_wrap("o.head", RK_DT_F32, hd, 1, c->attn->data,
	                       c->head_dim * sizeof(float));

	size_t hist = (size_t)c->n_layers * c->heads * c->context * c->head_dim;
	c->kc = kzalloc(hist * sizeof(float));
	c->vc = kzalloc(hist * sizeof(float));
	if (!c->kc || !c->vc)
		goto fail;

	c->kv = rk_tensor_wrap("k.hist", RK_DT_F32, full, 1, c->kc,
	                       (size_t)c->context * c->head_dim * sizeof(float));
	c->vv = rk_tensor_wrap("v.hist", RK_DT_F32, full, 1, c->vc,
	                       (size_t)c->context * c->head_dim * sizeof(float));
	if (!c->qv || !c->kv || !c->vv || !c->ov)
		goto fail;

	pr_info("%s ready: %u layers, dim %u, %u heads of %u, ffn %u, vocab %u, "
	        "context %u, %pB of history",
	        m->name, c->n_layers, c->embed, c->heads, c->head_dim, c->ffn,
	        c->vocab, c->context, RK_BYTES(hist * sizeof(float) * 2));
	return c;

fail:
	rk_llm_close(c);
	return NULL;
}

void rk_llm_close(struct rk_llm *c)
{
	if (!c)
		return;
	struct rk_tensor *t[] = { c->x, c->h, c->q, c->k, c->v, c->attn,
	                          c->g, c->u, c->logits, c->idv,
	                          c->qv, c->kv, c->vv, c->ov };
	for (unsigned i = 0; i < ARRAY_SIZE(t); i++)
		if (t[i])
			rk_tensor_put(t[i]);
	kfree(c->kc);
	kfree(c->vc);
	kfree(c);
}

/* ------------------------------------------------------------------ step */

/* One token in, one token out. The position is the context length so far, so
 * calling this repeatedly generates; calling it once per prompt token and
 * discarding the result is a prefill. */
int rk_llm_step(struct rk_llm *c, u32 token, u32 *next)
{
	if (!c || token >= c->vocab)
		return RK_EINVAL;
	if (c->pos >= c->context)
		return RK_ENOSPC;

	u64 t0 = rk_time_ns();
	struct rk_op_args a = { .eps = c->eps, .head_dim = c->head_dim,
	                        .pos = c->pos, .scale = 0.0f, .axis = -1 };
	int rc;

	/* Floating point is legal in this directory and nowhere else, and only
	 * inside this window. See kernel/core/fpu.c. */
	rk_fpu_begin();

	rk_tensor_set_f32(c->idv, 0, (float)token);
	rc = rk_op_exec(RK_OP_EMBED, c->x, c->tok_embd, c->idv, NULL, &a);
	if (rc != RK_OK)
		goto out;

	for (u32 l = 0; l < c->n_layers; l++) {
		/* --- attention ------------------------------------------------ */
		rc = rk_op_exec(RK_OP_RMSNORM, c->h, c->x, c->layer[l].attn_norm, NULL, &a);
		if (rc != RK_OK) goto out;

		rc = rk_op_exec(RK_OP_MATVEC, c->q, c->layer[l].wq, c->h, NULL, &a);
		if (rc == RK_OK)
			rc = rk_op_exec(RK_OP_MATVEC, c->k, c->layer[l].wk, c->h, NULL, &a);
		if (rc == RK_OK)
			rc = rk_op_exec(RK_OP_MATVEC, c->v, c->layer[l].wv, c->h, NULL, &a);
		if (rc != RK_OK) goto out;

		/* Rotary position encoding is applied to queries and keys and not to
		 * values: it encodes *where* a token is, and only the comparison
		 * between a query and a key cares. */
		rc = rk_op_exec(RK_OP_ROPE, c->q, NULL, NULL, NULL, &a);
		if (rc == RK_OK)
			rc = rk_op_exec(RK_OP_ROPE, c->k, NULL, NULL, NULL, &a);
		if (rc != RK_OK) goto out;

		/* Append this token's keys and values to the history, per head. */
		for (u32 hh = 0; hh < c->heads; hh++) {
			size_t base = (((size_t)l * c->heads + hh) * c->context + c->pos)
			              * c->head_dim;
			for (u32 d = 0; d < c->head_dim; d++) {
				c->kc[base + d] = rk_tensor_get_f32(c->k, hh * c->head_dim + d);
				c->vc[base + d] = rk_tensor_get_f32(c->v, hh * c->head_dim + d);
			}
		}

		/* Attend, one head at a time, by repointing the views rather than
		 * building new ones. Nothing is allocated in here. */
		for (u32 hh = 0; hh < c->heads; hh++) {
			size_t hbase = ((size_t)l * c->heads + hh) * c->context * c->head_dim;
			u64 seq = c->pos + 1;

			c->qv->data = (float *)c->q->data + hh * c->head_dim;
			c->ov->data = (float *)c->attn->data + hh * c->head_dim;

			c->kv->data     = c->kc + hbase;
			c->vv->data     = c->vc + hbase;
			c->kv->nelem    = c->vv->nelem    = seq * c->head_dim;
			c->kv->shape[0] = c->vv->shape[0] = seq * c->head_dim;

			rc = rk_op_exec(RK_OP_ATTENTION, c->ov, c->qv, c->kv, c->vv, &a);
			if (rc != RK_OK) goto out;
		}

		rc = rk_op_exec(RK_OP_MATVEC, c->h, c->layer[l].wo, c->attn, NULL, &a);
		if (rc != RK_OK) goto out;
		add_into(c->x, c->h);          /* residual */

		/* --- feed forward --------------------------------------------- */
		rc = rk_op_exec(RK_OP_RMSNORM, c->h, c->x, c->layer[l].ffn_norm, NULL, &a);
		if (rc == RK_OK)
			rc = rk_op_exec(RK_OP_MATVEC, c->g, c->layer[l].gate, c->h, NULL, &a);
		if (rc == RK_OK)
			rc = rk_op_exec(RK_OP_MATVEC, c->u, c->layer[l].up, c->h, NULL, &a);
		if (rc == RK_OK)
			rc = rk_op_exec(RK_OP_SILU, c->g, c->g, NULL, NULL, &a);
		if (rc == RK_OK)
			rc = rk_op_exec(RK_OP_MUL, c->g, c->g, c->u, NULL, &a);
		if (rc == RK_OK)
			rc = rk_op_exec(RK_OP_MATVEC, c->h, c->layer[l].down, c->g, NULL, &a);
		if (rc != RK_OK) goto out;
		add_into(c->x, c->h);          /* residual */
	}

	rc = rk_op_exec(RK_OP_RMSNORM, c->h, c->x, c->out_norm, NULL, &a);
	if (rc == RK_OK)
		rc = rk_op_exec(RK_OP_MATVEC, c->logits, c->out_w, c->h, NULL, &a);
	if (rc == RK_OK)
		rc = rk_op_exec(RK_OP_ARGMAX, c->idv, c->logits, NULL, NULL, &a);
	if (rc != RK_OK)
		goto out;

	if (next)
		*next = (u32)rk_tensor_get_f32(c->idv, 0);

	c->pos++;
	c->tokens++;

out:
	rk_fpu_end();
	c->compute_ns += rk_time_ns() - t0;
	return rc;
}

u32 rk_llm_position(const struct rk_llm *c) { return c ? c->pos : 0; }

void rk_llm_reset(struct rk_llm *c)
{
	if (!c)
		return;
	c->pos = 0;
	size_t hist = (size_t)c->n_layers * c->heads * c->context * c->head_dim;
	memset(c->kc, 0, hist * sizeof(float));
	memset(c->vc, 0, hist * sizeof(float));
}

/* ------------------------------------------------------------- generation */

/* Generate `count` tokens, starting from `seed`, yielding between each.
 *
 * The yield is the entire point of SCHED_INFERENCE and is why this loop is in
 * the kernel rather than being a tight loop somewhere that cannot be
 * preempted: a generation that never yields makes the machine unusable, and
 * one that yields every token does not. */
int rk_llm_generate(struct rk_llm *c, u32 seed, u32 count,
                    u32 *out, u32 *produced)
{
	if (!c || !count)
		return RK_EINVAL;

	u32 tok = seed % c->vocab;
	u32 n = 0;
	int rc = RK_OK;

	for (u32 i = 0; i < count; i++) {
		u32 next = 0;
		rc = rk_llm_step(c, tok, &next);
		if (rc != RK_OK)
			break;
		if (out)
			out[n] = next;
		n++;
		tok = next;

		rk_graph_record(GEV_INFERENCE, c->model->id, tok, c->pos,
		                c->compute_ns);
		sched_yield();
	}

	if (produced)
		*produced = n;
	return n ? RK_OK : rc;
}

void rk_llm_stats(const struct rk_llm *c, u64 *tokens, u64 *compute_ns)
{
	if (tokens)
		*tokens = c ? c->tokens : 0;
	if (compute_ns)
		*compute_ns = c ? c->compute_ns : 0;
}

/* ------------------------------------------------------------------ demo */

/* Load the fixture model and generate from it on a thread in the inference
 * class, with a declared rate and a per-token budget.
 *
 * This is the demonstration the whole design points at, so it is worth being
 * precise about what it shows and what it does not. It shows that a model can
 * be loaded from a filesystem, that a real forward pass runs through the
 * kernel's own operators, and that the work is admitted, budgeted and yielded
 * as a soft real-time stream rather than as a tight loop. It shows nothing
 * whatsoever about model quality: the weights are pseudo-random and the tokens
 * are meaningless by construction. */

struct demo_arg {
	struct rk_llm *llm;
	u32 count;
	u32 produced;
	u64 ns;
	int rc;
};

static void demo_thread(void *arg)
{
	struct demo_arg *d = arg;
	u64 t0 = rk_time_ns();
	d->rc = rk_llm_generate(d->llm, 1, d->count, NULL, &d->produced);
	d->ns = rk_time_ns() - t0;
	thread_exit(0);
}

int rk_llm_demo(const char *path, u32 count, u32 rate_hz)
{
	if (!count)
		count = 16;
	if (!rate_hz)
		rate_hz = 50;

	struct rk_model *m = rk_model_find("tiny");
	if (!m) {
		m = rk_model_load(path, "tiny");
		if (!m) {
			pr_warn("no model at %s", path);
			return RK_ENOENT;
		}
	}

	struct rk_llm *c = rk_llm_open(m, 0);
	if (!c)
		return RK_ENOTSUP;

	struct demo_arg d = { .llm = c, .count = count };

	struct thread *t = thread_create("infer", demo_thread, &d,
	                                 SCHED_INFERENCE, 8);
	if (!t) {
		rk_llm_close(c);
		return RK_ENOMEM;
	}

	/* A declared rate is what makes this admissible rather than best effort.
	 * The budget is deliberately generous: the point is that a budget exists
	 * and is enforced, not that this particular model fits a tight one. */
	/* A quarter of the period, not half: admission control refuses anything
	 * that would overcommit the class, and a task that asks for more than it
	 * needs gets refused and silently falls back to best effort - which is
	 * correct behaviour and a poor demonstration of it. */
	u64 period = RK_NS_PER_S / rate_hz;
	int admitted = sched_set_deadline(t, period, period / 4);
	thread_start(t);
	thread_join(t, NULL);

	u64 tokens = 0, compute = 0;
	rk_llm_stats(c, &tokens, &compute);

	struct rk_infer_stats is;
	rk_infer_stats(&is);

	if (d.produced) {
		u64 per_token = d.ns / d.produced;
		rk_printf("  model          %s (%u layers, dim %u, vocab %u)\n",
		          m->name, m->n_layers, m->embed_dim, m->vocab_size);
		rk_printf("  generated      %u tokens in %llu ms\n",
		          d.produced, (unsigned long long)(d.ns / 1000000));
		rk_printf("  per token      %llu us\n",
		          (unsigned long long)(per_token / 1000));
		rk_printf("  rate           %llu tokens/sec\n",
		          (unsigned long long)(per_token ? 1000000000ull / per_token : 0));
		rk_printf("  class          SCHED_INFERENCE, %u Hz declared, "
		          "%llu us budget, %s\n",
		          rate_hz, (unsigned long long)(period / 4000),
		          admitted == RK_OK ? "admitted"
		                            : "refused, running best effort");
		rk_printf("  deadline miss  %llu\n",
		          (unsigned long long)is.deadline_misses);
	} else {
		rk_printf("  the model produced nothing: %s\n", rk_strerror(d.rc));
	}

	rk_llm_close(c);
	return d.produced ? RK_OK : d.rc;
}
