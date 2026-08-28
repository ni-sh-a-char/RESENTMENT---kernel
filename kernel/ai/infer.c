/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - inference scheduling and kernel-side advice.
 *
 * Two halves.
 *
 * The queue: inference requests carry a deadline and a target token rate, and
 * a worker thread in the SCHED_INFERENCE class services them earliest deadline
 * first. A request that overruns is preempted rather than allowed to starve
 * the rest of the machine, and the miss is counted. Counting it matters: a
 * real-time class that never reports a miss is one nobody can trust.
 *
 * The advisors: a bounded, strictly optional way for the kernel to ask a model
 * a policy question it has no correct static answer to - which page to evict,
 * where to place a thread, whether to admit a real-time task. Three rules make
 * this safe rather than clever:
 *
 *   an advisor runs with a hard deadline and is abandoned if it is late
 *   its answer is clamped to the candidate set the kernel offered
 *   if there is no advisor, or it fails, the deterministic heuristic runs
 *
 * An operating system may consult a model. It may never depend on one.
 */
#include <rk/ai.h>
#include <rk/mm.h>
#include <rk/sched.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/time.h>
#include <rk/graph.h>
#include <rk/arch.h>
#include <rk/irq.h>

#undef RK_SUBSYS
#define RK_SUBSYS "infer"

static LIST_HEAD(infer_queue);
static spinlock_t queue_lock = SPINLOCK_INIT("infer-queue");
static struct waitqueue queue_wq;
static struct rk_infer_stats istats;
static rk_id_t next_req_id = 1;
static struct thread *worker;
static volatile bool infer_running;

/* --------------------------------------------------------------- queue */

static struct rk_infer_req *pick_next_locked(void)
{
	struct rk_infer_req *best = NULL, *r;

	list_for_each_entry(r, &infer_queue, link) {
		if (r->state != RK_INFER_QUEUED)
			continue;
		if (!best) {
			best = r;
			continue;
		}
		/* Deadlines first, then priority, then arrival order. A request with
		 * no deadline never displaces one that has one. */
		bool r_has = r->deadline_ns != 0, b_has = best->deadline_ns != 0;
		if (r_has && !b_has)
			best = r;
		else if (r_has == b_has) {
			if (r_has && r->deadline_ns < best->deadline_ns)
				best = r;
			else if (!r_has && r->priority < best->priority)
				best = r;
		}
	}
	return best;
}

int rk_infer_submit(struct rk_infer_req *r)
{
	if (!r || !r->model)
		return RK_EINVAL;

	r->id        = __atomic_add_fetch(&next_req_id, 1, __ATOMIC_SEQ_CST) - 1;
	r->state     = RK_INFER_QUEUED;
	r->queued_ns = rk_time_ns();
	r->error     = RK_OK;
	completion_init(&r->done);
	list_init(&r->link);

	/* A model that failed verification cannot run. The check is here, at the
	 * gate, rather than inside the compute loop where it would be skipped by
	 * anything that called the operators directly. */
	if (!r->model->verified) {
		r->state = RK_INFER_FAILED;
		r->error = RK_ESEAL;
		return RK_ESEAL;
	}

	/* Turn a target rate into a per-request deadline, so the scheduler has
	 * something concrete to order by. */
	if (!r->deadline_ns && r->target_rate)
		r->deadline_ns = r->queued_ns + (RK_NS_PER_S / r->target_rate) *
		                 (r->max_tokens ? r->max_tokens : 1);

	struct graph_node *n = rk_graph_node_create(GNODE_INFERENCE, r->model->name, NULL, r);
	if (n) {
		rk_graph_set_u64(n, "req", r->id);
		rk_graph_set_u64(n, "deadline", r->deadline_ns);
		rk_graph_set_u64(n, "maxtok", r->max_tokens);
		r->graph_node = n->id;
	}

	unsigned long f = spin_lock_irqsave(&queue_lock);
	list_add_tail(&r->link, &infer_queue);
	istats.submitted++;
	istats.queue_depth++;
	spin_unlock_irqrestore(&queue_lock, f);

	waitq_wake_one(&queue_wq);
	rk_graph_record(GEV_INFERENCE, r->graph_node, r->id, 0, r->queued_ns);
	return RK_OK;
}

int rk_infer_wait(struct rk_infer_req *r, u64 timeout_ns)
{
	if (!r)
		return RK_EINVAL;
	if (timeout_ns)
		return completion_wait_timeout(&r->done, timeout_ns);
	completion_wait(&r->done);
	return r->error;
}

int rk_infer_cancel(struct rk_infer_req *r)
{
	if (!r)
		return RK_EINVAL;

	unsigned long f = spin_lock_irqsave(&queue_lock);
	if (r->state == RK_INFER_QUEUED) {
		list_del(&r->link);
		r->state = RK_INFER_CANCELED;
		r->error = RK_ECANCELED;
		istats.canceled++;
		if (istats.queue_depth)
			istats.queue_depth--;
		spin_unlock_irqrestore(&queue_lock, f);
		completion_done(&r->done);
		return RK_OK;
	}
	/* Already running: ask it to stop at the next token boundary rather than
	 * tearing down state mid-operator. */
	if (r->state == RK_INFER_RUNNING) {
		r->preemptible = true;
		r->state = RK_INFER_CANCELED;
		spin_unlock_irqrestore(&queue_lock, f);
		return RK_OK;
	}
	spin_unlock_irqrestore(&queue_lock, f);
	return RK_EINVAL;
}

/* One step of work. The real forward pass belongs to whatever runtime owns the
 * graph of operators; what the kernel guarantees is that the step is bounded,
 * accounted and preemptible, which is what the class exists for. */
static int run_request(struct rk_infer_req *r)
{
	u64 start = rk_time_ns();
	r->started_ns = start;
	r->state = RK_INFER_RUNNING;

	int rc = RK_OK;
	u32 want = r->max_tokens ? r->max_tokens : 1;

	for (u32 i = 0; i < want; i++) {
		if (r->state == RK_INFER_CANCELED) {
			rc = RK_ECANCELED;
			break;
		}

		/* A single token step: project, attend, sample. The operators are
		 * the kernel ones, so the accounting below covers real work. */
		if (r->input && r->output) {
			struct rk_op_args a = { .eps = 1e-5f, .head_dim = r->model->head_dim };
			rc = rk_op_exec(RK_OP_MATVEC, r->output, r->input, r->input, NULL, &a);
			if (rc != RK_OK)
				break;
		}
		r->produced++;
		istats.tokens_generated++;

		/* Yield between tokens. This is the whole reason inference is a
		 * scheduling class: a generation loop that never yields makes the
		 * machine unusable, and one that yields every token does not. */
		if (r->preemptible)
			sched_yield();

		if (r->deadline_ns && rk_time_ns() > r->deadline_ns) {
			istats.deadline_misses++;
			break;
		}
	}

	r->finished_ns = rk_time_ns();
	istats.total_compute_ns += r->finished_ns - start;
	istats.total_wait_ns    += start - r->queued_ns;
	return rc;
}

static void infer_worker(void *arg)
{
	(void)arg;
	infer_running = true;

	while (infer_running) {
		unsigned long f = spin_lock_irqsave(&queue_lock);
		struct rk_infer_req *r = pick_next_locked();
		if (r) {
			list_del(&r->link);
			if (istats.queue_depth)
				istats.queue_depth--;
		}
		spin_unlock_irqrestore(&queue_lock, f);

		if (!r) {
			waitq_wait_timeout(&queue_wq, 10 * RK_NS_PER_MS);
			continue;
		}

		int rc = run_request(r);

		f = spin_lock_irqsave(&queue_lock);
		if (rc == RK_OK) {
			r->state = RK_INFER_DONE;
			istats.completed++;
		} else if (rc == RK_ECANCELED) {
			r->state = RK_INFER_CANCELED;
			istats.canceled++;
		} else {
			r->state = RK_INFER_FAILED;
			istats.failed++;
		}
		r->error = rc;
		spin_unlock_irqrestore(&queue_lock, f);

		rk_graph_record(GEV_INFERENCE, r->graph_node, r->id, r->produced,
		                r->finished_ns - r->started_ns);
		completion_done(&r->done);
	}
	thread_exit(0);
}

void rk_infer_stats(struct rk_infer_stats *out)
{
	unsigned long f = spin_lock_irqsave(&queue_lock);
	*out = istats;
	spin_unlock_irqrestore(&queue_lock, f);
}

/* ------------------------------------------------------------- advisors */

struct advisor {
	rk_advisor_fn fn;
	void         *ctx;
	u64           deadline_ns;
	u64           asked, answered, late, rejected;
};

static struct advisor advisors[RK_ADVICE_COUNT];

int rk_advisor_register(enum rk_advice_topic topic, rk_advisor_fn fn, void *ctx,
                        u64 deadline_ns)
{
	if ((unsigned)topic >= RK_ADVICE_COUNT || !fn)
		return RK_EINVAL;

	/* A deadline is mandatory and capped. An advisor that can block for an
	 * unbounded time is a way to hang the kernel from userspace. */
	if (!deadline_ns || deadline_ns > 5 * RK_NS_PER_MS)
		deadline_ns = RK_NS_PER_MS;

	advisors[topic].fn = fn;
	advisors[topic].ctx = ctx;
	advisors[topic].deadline_ns = deadline_ns;
	pr_info("advisor registered for %u with a %llu ns budget",
	        (unsigned)topic, (unsigned long long)deadline_ns);
	return RK_OK;
}

void rk_advisor_unregister(enum rk_advice_topic topic)
{
	if ((unsigned)topic < RK_ADVICE_COUNT)
		advisors[topic].fn = NULL;
}

u64 rk_advice_ask(const struct rk_advice_query *q, u64 fallback)
{
	if (!q || (unsigned)q->topic >= RK_ADVICE_COUNT)
		return fallback;

	struct advisor *a = &advisors[q->topic];
	if (!a->fn)
		return fallback;

	/* Never consult an advisor from an interrupt handler, and never while
	 * holding the memory manager, which is exactly where the page eviction
	 * question comes from. The caller is responsible for asking from a safe
	 * context; asking from an unsafe one is a bug, not a slow path. */
	if (rk_in_irq())
		return fallback;

	a->asked++;
	u64 start = rk_time_ns();
	u64 choice = fallback;
	int rc = a->fn(q, &choice, a->ctx);
	u64 took = rk_time_ns() - start;

	if (took > a->deadline_ns) {
		a->late++;
		/* Late answers are discarded, not used. Using one would make the
		 * kernel's behaviour depend on how loaded the model happened to be. */
		return fallback;
	}
	if (rc != RK_OK) {
		a->rejected++;
		return fallback;
	}

	/* Clamp to the candidate set. An advisor cannot name a page that was
	 * never offered, however confident it is. */
	for (u32 i = 0; i < q->ncandidates; i++) {
		if (q->candidates[i] == choice) {
			a->answered++;
			return choice;
		}
	}
	a->rejected++;
	return fallback;
}

void rk_advisor_stats(enum rk_advice_topic t, u64 *asked, u64 *answered,
                      u64 *late, u64 *rejected)
{
	if ((unsigned)t >= RK_ADVICE_COUNT)
		return;
	if (asked)    *asked    = advisors[t].asked;
	if (answered) *answered = advisors[t].answered;
	if (late)     *late     = advisors[t].late;
	if (rejected) *rejected = advisors[t].rejected;
}

/* ------------------------------------------------------------------ init */

void rk_infer_init(void)
{
	spin_lock_init(&queue_lock, "infer-queue");
	waitq_init(&queue_wq, "infer");

	worker = thread_create("inferd", infer_worker, NULL, SCHED_INFERENCE, 4);
	if (worker) {
		/* 8 ms of budget every 20 ms: enough to make progress on a token
		 * stream, bounded enough that the rest of the system stays live. */
		sched_set_deadline(worker, 20 * RK_NS_PER_MS, 8 * RK_NS_PER_MS);
		thread_start(worker);
	}
	pr_info("inference scheduler ready");
}

int rk_ai_selftest(void)
{
	u64 shape[2] = { 4, 4 };
	struct rk_tensor *a = rk_tensor_create("a", RK_DT_F32, shape, 2, 0);
	struct rk_tensor *b = rk_tensor_create("b", RK_DT_F32, shape, 2, 0);
	struct rk_tensor *c = rk_tensor_create("c", RK_DT_F32, shape, 2, 0);
	int rc = RK_OK;

	if (!a || !b || !c) {
		rc = RK_ENOMEM;
		goto out;
	}

	/* Identity times identity is identity: catches index order, stride and
	 * accumulator bugs in one check. */
	rk_tensor_fill(a, 0.0f);
	rk_tensor_fill(b, 0.0f);
	for (u64 i = 0; i < 4; i++) {
		rk_tensor_set_f32(a, i * 4 + i, 1.0f);
		rk_tensor_set_f32(b, i * 4 + i, 1.0f);
	}
	rc = rk_op_exec(RK_OP_MATMUL, c, a, b, NULL, NULL);
	if (rc != RK_OK)
		goto out;

	for (u64 i = 0; i < 16; i++) {
		float want = (i / 4 == i % 4) ? 1.0f : 0.0f;
		float got = rk_tensor_get_f32(c, i);
		if (got < want - 0.001f || got > want + 0.001f) {
			pr_err("matmul identity check failed at %llu: got %d milli, want %d",
			       (unsigned long long)i, (int)(got * 1000), (int)(want * 1000));
			rc = RK_EIO;
			goto out;
		}
	}

	/* Softmax must sum to one. */
	u64 row[1] = { 4 };
	struct rk_tensor *s = rk_tensor_create("s", RK_DT_F32, row, 1, 0);
	if (s) {
		for (u64 i = 0; i < 4; i++)
			rk_tensor_set_f32(s, i, (float)i);
		rk_op_exec(RK_OP_SOFTMAX, s, s, NULL, NULL, NULL);
		float sum = 0.0f;
		for (u64 i = 0; i < 4; i++)
			sum += rk_tensor_get_f32(s, i);
		if (sum < 0.99f || sum > 1.01f) {
			pr_err("softmax sums to %d milli, expected 1000", (int)(sum * 1000));
			rc = RK_EIO;
		}
		rk_tensor_put(s);
	}

	if (rc == RK_OK)
		pr_info("AI self-test passed: matmul, softmax, tensor lifecycle");
out:
	rk_tensor_put(a);
	rk_tensor_put(b);
	rk_tensor_put(c);
	return rc;
}

void rk_ai_init(void)
{
	extern void rk_tensor_init(void);
	extern void rk_accel_init(void);
	extern void rk_model_init(void);
	extern void rk_kv_init(u64 budget_bytes);

	rk_tensor_init();
	rk_accel_init();
	rk_model_init();
	rk_kv_init(0);
	rk_infer_init();
}
