/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the scheduler.
 *
 * Four classes, strictly ordered. The interesting one is SCHED_INFERENCE.
 *
 * A token generation loop is not interactive: it does not sleep waiting for a
 * human, so a sleep-credit heuristic gives it nothing. It is not batch either:
 * it has a rate the user can see, and missing it looks like the machine
 * stuttering. It is a soft real-time stream with a deadline per token and a
 * budget per period, so that is exactly how it is scheduled - admitted with a
 * declared rate, given a budget, and demoted rather than dropped when it
 * overruns. Treating inference as batch is the single most common reason a
 * model that benchmarks well feels bad to use.
 *
 * Everything here is deterministic given the same event sequence, which is
 * what lets a scheduling trace be replayed from the runtime graph.
 */
#include <rk/sched.h>
#include <rk/mm.h>
#include <rk/arch.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/panic.h>
#include <rk/time.h>
#include <rk/graph.h>
#include <rk/cap.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "sched"

#define SLICE_MIN_NS   (1ull * RK_NS_PER_MS)
#define SLICE_BASE_NS  (4ull * RK_NS_PER_MS)
#define SLICE_MAX_NS   (40ull * RK_NS_PER_MS)

/* One shared ready queue, and per-CPU state beside it.
 *
 * A shared queue rather than per-CPU queues with work stealing. The trade is
 * deliberate: a shared queue is one lock and is obviously correct, and it
 * balances perfectly by construction because any idle core takes the next
 * runnable thread. Per-CPU queues win on machines with enough cores that the
 * lock becomes the bottleneck, and they cost a stealing policy, an imbalance
 * metric and a whole class of subtle bug. This is the right end of that trade
 * for now.
 *
 * ponytail: global run queue. Split it per-CPU when contention on rq.lock
 * actually shows up in a profile, not before. */
struct runqueue {
	struct list_head ready[SCHED_NCLASSES][RK_PRIO_LEVELS];
	u64              bitmap[SCHED_NCLASSES];   /* which priorities are non-empty */
	spinlock_t       lock;
	u64              nready;
	u64              switches;
};

struct cpu_state {
	struct thread *current;
	struct thread *idle;
	/* The thread this core switched away from and still owes the run queue.
	 * It cannot be requeued before the switch: until the stack pointer has
	 * actually been saved, another core that picked it up would be running on
	 * a stack this core is still using. */
	struct thread *prev_pending;
	void          *discard_sp;   /* somewhere harmless for the first switch */
	u64            idle_ns;
	u32            preempt_disable;
	bool           need_resched;
	bool           online;
	volatile bool  idling;      /* halted, waiting for anything to happen */
};

static struct runqueue  rq_shared;
static struct cpu_state cpu_state[RK_MAX_CPUS];
static bool            sched_ready;
static rk_id_t         next_tid = 1;
static rk_id_t         next_pid = 1;
static struct task     kernel_task;
static LIST_HEAD(all_tasks);
static LIST_HEAD(all_threads);
static DEFINE_SPINLOCK(global_lock);
static struct sched_stats gstats;
static struct kmem_cache *thread_cache;
static struct kmem_cache *task_cache;

static const char *const class_names[SCHED_NCLASSES] = {
	"realtime", "inference", "interactive", "batch"
};
static const char *const state_names[] = {
	"new", "ready", "running", "blocked", "sleeping", "zombie", "dead"
};

const char *sched_class_name(enum sched_class c)
{
	return (unsigned)c < SCHED_NCLASSES ? class_names[c] : "?";
}

const char *thread_state_name(enum thread_state s)
{
	return (unsigned)s < ARRAY_SIZE(state_names) ? state_names[s] : "?";
}

bool sched_active(void) { return sched_ready; }

static inline struct runqueue *this_rq(void) { return &rq_shared; }

static void kick_idle_cpu(u32 self);

static inline struct cpu_state *this_cpu_state(void)
{
	u32 id = arch_cpu_id();
	return &cpu_state[id < RK_MAX_CPUS ? id : 0];
}

/* ---------------------------------------------------------- ready queues */

static void rq_enqueue(struct runqueue *rq, struct thread *t)
{
	u32 p = t->priority < RK_PRIO_LEVELS ? t->priority : RK_PRIO_LEVELS - 1;
	list_add_tail(&t->runq_link, &rq->ready[t->sclass][p]);
	rq->bitmap[t->sclass] |= 1ull << p;
	rq->nready++;
	t->state = THREAD_READY;
}

static void rq_dequeue(struct runqueue *rq, struct thread *t)
{
	u32 p = t->priority < RK_PRIO_LEVELS ? t->priority : RK_PRIO_LEVELS - 1;
	list_del(&t->runq_link);
	if (list_empty(&rq->ready[t->sclass][p]))
		rq->bitmap[t->sclass] &= ~(1ull << p);
	if (rq->nready)
		rq->nready--;
}

/* May this core run this thread? Affinity is normally "anywhere", so this is
 * a single test that almost always passes, but it has to be checked in every
 * picker or setting an affinity would be advisory. */
static inline bool runnable_here(const struct thread *t)
{
	return (t->affinity_mask & (1ull << (arch_cpu_id() & 63))) != 0;
}

/* Earliest deadline first within the two deadline classes. Scanning the whole
 * priority band is fine: these queues are short by construction, because
 * admission control refuses to overcommit them. */
static struct thread *pick_deadline(struct runqueue *rq, enum sched_class cls)
{
	struct thread *best = NULL;
	for (u32 p = 0; p < RK_PRIO_LEVELS; p++) {
		if (!(rq->bitmap[cls] & (1ull << p)))
			continue;
		struct thread *t;
		list_for_each_entry(t, &rq->ready[cls][p], runq_link) {
			if (!runnable_here(t))
				continue;
			if (!best || t->deadline_ns < best->deadline_ns)
				best = t;
		}
	}
	return best;
}

static struct thread *pick_priority(struct runqueue *rq, enum sched_class cls)
{
	for (u32 p = 0; p < RK_PRIO_LEVELS; p++) {
		if (!(rq->bitmap[cls] & (1ull << p)))
			continue;
		struct thread *t;
		list_for_each_entry(t, &rq->ready[cls][p], runq_link)
			if (runnable_here(t))
				return t;
	}
	return NULL;
}

/* Weighted fair share: pick the thread with the smallest virtual runtime, so
 * a low-weight thread advances its clock faster and yields sooner. */
static struct thread *pick_fair(struct runqueue *rq)
{
	struct thread *best = NULL;
	for (u32 p = 0; p < RK_PRIO_LEVELS; p++) {
		if (!(rq->bitmap[SCHED_BATCH] & (1ull << p)))
			continue;
		struct thread *t;
		list_for_each_entry(t, &rq->ready[SCHED_BATCH][p], runq_link)
			if (runnable_here(t) && (!best || t->vruntime < best->vruntime))
				best = t;
	}
	return best;
}

static struct thread *pick_next(struct runqueue *rq)
{
	struct thread *t;

	if ((t = pick_deadline(rq, SCHED_REALTIME)))
		return t;

	/* Inference before interactive, but only while it is inside its budget.
	 * An overrunning generation loop is demoted for the rest of its period
	 * rather than allowed to starve the UI - a slow answer beats a frozen
	 * machine, and this is the line between the two. */
	if ((t = pick_deadline(rq, SCHED_INFERENCE))) {
		if (t->budget_left_ns > 0 || t->budget_ns == 0)
			return t;
	}
	if ((t = pick_priority(rq, SCHED_INTERACTIVE)))
		return t;
	if ((t = pick_fair(rq)))
		return t;
	/* Everything else is exhausted; run the overrunning inference thread
	 * rather than idling, since the CPU would otherwise be wasted. */
	if (t == NULL && (t = pick_deadline(rq, SCHED_INFERENCE)))
		return t;
	return NULL;   /* nothing runnable; the caller falls back to its idle */
}

static u64 slice_for(const struct thread *t)
{
	switch (t->sclass) {
	case SCHED_REALTIME:
		return t->budget_ns ? t->budget_ns : SLICE_MAX_NS;
	case SCHED_INFERENCE:
		/* A slice long enough to produce several tokens amortises the cache
		 * refill that a switch costs; too long and interactivity suffers. */
		return t->budget_ns ? MIN(t->budget_ns, SLICE_MAX_NS) : (8 * RK_NS_PER_MS);
	case SCHED_INTERACTIVE:
		/* Higher priority means a shorter slice: responsive, not greedy. */
		return SLICE_BASE_NS + (u64)t->priority * (RK_NS_PER_MS / 2);
	default:
		return SLICE_MAX_NS;
	}
}

/* ------------------------------------------------------------- switching */

/* Called on the far side of every context switch, including the first
 * instruction a brand-new thread ever executes, which is why the architecture
 * trampolines call it too. Its whole job is to put the thread this core just
 * stopped running back on the run queue, now that its stack pointer has been
 * saved and another core may safely take it. */
void sched_finish_switch(void)
{
	struct cpu_state *cs = this_cpu_state();
	struct thread *p = cs->prev_pending;
	if (!p)
		return;
	cs->prev_pending = NULL;

	unsigned long flags = spin_lock_irqsave(&rq_shared.lock);
	rq_enqueue(&rq_shared, p);
	spin_unlock_irqrestore(&rq_shared.lock, flags);
}

static void context_switch(struct cpu_state *cs, struct thread *prev, struct thread *next)
{
	if (prev == next)
		return;

	u64 now = rk_time_ns();
	if (prev && prev->last_start_ns) {
		u64 ran = now - prev->last_start_ns;
		prev->runtime_ns += ran;
		if (prev->task)
			prev->task->cpu_time_ns += ran;
		if (prev->sclass == SCHED_BATCH) {
			u32 w = prev->weight ? prev->weight : 1024;
			prev->vruntime += (ran * 1024) / w;
		}
		if (prev->sclass == SCHED_INFERENCE && prev->budget_left_ns)
			prev->budget_left_ns = ran >= prev->budget_left_ns ? 0
			                                                   : prev->budget_left_ns - ran;
	}

	next->last_start_ns = now;
	next->state = THREAD_RUNNING;
	next->cpu   = arch_cpu_id();
	cs->current = next;
	rq_shared.switches++;
	gstats.switches++;
	next->event_seq++;

	rk_graph_record(GEV_SCHED_SWITCH, next->tid,
	                prev ? prev->tid : 0, next->tid, now);

	if (prev && prev->fpu_state)
		arch_fpu_save(prev);

	arch_set_current_thread(next);
	if (next->pgtable && (!prev || prev->pgtable != next->pgtable))
		arch_pgtable_switch(next->pgtable);

	/* The very first switch has no previous context to save; give the arch
	 * layer somewhere harmless to write instead of a special case. */
	arch_context_switch(prev ? &prev->sp : &cs->discard_sp, next->sp);

	/* Back here means we have been scheduled again, possibly on a different
	 * core. Hand back whatever thread this core switched away from. */
	sched_finish_switch();

	struct thread *self = arch_current_thread();
	if (self && self->fpu_state)
		arch_fpu_restore(self);
}

/* The one place a context switch happens.
 *
 * The run queue lock is released *before* the switch, not held across it. The
 * usual trick of letting the next thread drop the previous thread's lock does
 * not work here, because a brand-new thread starts at its trampoline and never
 * returns into the code that took the lock - so the very first time the
 * scheduler switched into a new thread, the run queue would stay locked
 * forever and the next scheduling decision would spin.
 *
 * Interrupts are disabled for the whole function instead, which on this kernel
 * is sufficient: nothing else on this CPU can touch the queue, and a second
 * CPU cannot reach a thread that is still marked running.
 *
 * prev_state is applied under the lock rather than by the caller, so a wakeup
 * arriving between "I am about to block" and the switch cannot be lost.
 */
static void __schedule(u32 prev_state)
{
	if (!sched_ready)
		return;

	unsigned long flags = arch_irq_save();
	struct runqueue *rq = this_rq();
	struct cpu_state *cs = this_cpu_state();

	spin_lock(&rq->lock);

	struct thread *prev = cs->current;
	if (prev && prev != cs->idle && prev_state != THREAD_RUNNING)
		prev->state = prev_state;

	struct thread *next = pick_next(rq);
	if (next)
		rq_dequeue(rq, next);
	else
		next = cs->idle;

	/* A thread that is still runnable goes back on the queue - but only once
	 * the switch has finished, see prev_pending. One that blocked or exited
	 * does not go back at all. */
	if (prev && prev != cs->idle && prev->state == THREAD_RUNNING)
		cs->prev_pending = prev;

	next->slice_ns = slice_for(next);
	cs->need_resched = false;

	spin_unlock(&rq->lock);

	if (next != prev)
		context_switch(cs, prev, next);

	arch_irq_restore(flags);
}

void sched_yield(void)
{
	struct thread *t = arch_current_thread();
	if (t)
		t->nvcsw++;
	__schedule(THREAD_RUNNING);
}

void sched_preempt_disable(void) { this_cpu_state()->preempt_disable++; }

void sched_preempt_enable(void)
{
	struct cpu_state *cs = this_cpu_state();
	if (cs->preempt_disable)
		cs->preempt_disable--;
	if (!cs->preempt_disable && cs->need_resched)
		sched_yield();
}

void sched_tick(void)
{
	if (!sched_ready)
		return;
	struct cpu_state *cs = this_cpu_state();
	struct thread *t = cs->current;
	if (!t)
		return;

	u64 now = rk_time_ns();

	/* Wake anything whose sleep expired. Done on the tick rather than with a
	 * timer per sleeper: sleepers are common and timers are not free. */
	struct thread *s, *tmp;
	unsigned long gf = spin_lock_irqsave(&global_lock);
	list_for_each_entry_safe(s, tmp, &all_threads, task_link) {
		if (s->state == THREAD_SLEEPING && s->sleep_until_ns <= now) {
			s->state = THREAD_READY;
			s->sleep_until_ns = 0;
			unsigned long rf = spin_lock_irqsave(&rq_shared.lock);
			rq_enqueue(&rq_shared, s);
			spin_unlock_irqrestore(&rq_shared.lock, rf);
		}
	}
	spin_unlock_irqrestore(&global_lock, gf);

	if (t == cs->idle) {
		cs->need_resched = true;
	} else if (t->last_start_ns && now - t->last_start_ns >= t->slice_ns) {
		t->nivcsw++;
		gstats.preemptions++;
		cs->need_resched = true;
	}

	/* Deadline accounting is separate from the slice: a task can be inside
	 * its slice and still have blown its deadline. */
	if ((t->sclass == SCHED_REALTIME || t->sclass == SCHED_INFERENCE) &&
	    t->deadline_ns && now > t->deadline_ns) {
		gstats.deadline_misses++;
		if (t->period_ns) {
			t->deadline_ns    = now + t->period_ns;
			t->budget_left_ns = t->budget_ns;
		}
	}

	if (cs->need_resched && !cs->preempt_disable)
		__schedule(THREAD_RUNNING);
}

void sched_block(void)
{
	struct thread *t = arch_current_thread();
	if (t)
		t->nvcsw++;
	__schedule(THREAD_BLOCKED);
}

void sched_wake(struct thread *t)
{
	if (!t || t->state == THREAD_READY || t->state == THREAD_RUNNING)
		return;
	if (t->state == THREAD_ZOMBIE || t->state == THREAD_DEAD)
		return;

	struct runqueue *rq = &rq_shared;
	unsigned long f = spin_lock_irqsave(&rq->lock);
	t->wakeup_ns = rk_time_ns();

	/* Interactive threads that just woke get a temporary boost, so a keypress
	 * beats a batch job that has been hogging the CPU. The boost decays back
	 * to the base priority as the thread uses CPU. */
	if (t->sclass == SCHED_INTERACTIVE && t->priority > 0)
		t->priority--;

	rq_enqueue(rq, t);
	spin_unlock_irqrestore(&rq->lock, f);

	/* A higher-priority wakeup should not wait for the running thread's slice
	 * to expire. Nudging every core is cheap and correct; picking the best one
	 * would need a policy that is not obviously better than letting whichever
	 * core reaches the scheduler first take it. */
	struct cpu_state *cs = this_cpu_state();
	if (cs->current && cs->current->sclass > t->sclass)
		cs->need_resched = true;

	kick_idle_cpu(arch_cpu_id());
}

void sched_sleep_ns(u64 ns)
{
	if (!sched_ready) {
		rk_udelay(ns / 1000);
		return;
	}
	struct thread *t = thread_current();
	if (!t)
		return;
	t->sleep_until_ns = rk_time_ns() + ns;
	t->nvcsw++;
	__schedule(THREAD_SLEEPING);
}

void sched_sleep_ms(u64 ms) { sched_sleep_ns(ms * RK_NS_PER_MS); }

/* ----------------------------------------------------------- thread API */

struct thread *thread_current(void) { return arch_current_thread(); }

struct task *task_current(void)
{
	struct thread *t = arch_current_thread();
	return t ? t->task : &kernel_task;
}

struct task *task_kernel(void) { return &kernel_task; }

struct thread *thread_create_in(struct task *task, const char *name,
                                void (*entry)(void *), void *arg,
                                enum sched_class cls, u32 priority)
{
	struct thread *t = kmem_cache_alloc(thread_cache);
	if (!t)
		return NULL;
	memset(t, 0, sizeof(*t));

	t->kstack = (vaddr_t)kmalloc_aligned(RK_KSTACK_SIZE, 16);
	if (!t->kstack) {
		kmem_cache_free(thread_cache, t);
		return NULL;
	}
	t->kstack_size = RK_KSTACK_SIZE;

	/* Poison the stack so an overflow is visible in a dump and an
	 * uninitialised read is not quietly zero. */
	memset((void *)t->kstack, 0xA5, RK_KSTACK_SIZE);

	t->tid    = __atomic_add_fetch(&next_tid, 1, __ATOMIC_SEQ_CST) - 1;
	t->task   = task ? task : &kernel_task;
	t->sclass = cls;
	t->priority = t->base_priority = priority < RK_PRIO_LEVELS ? priority : RK_PRIO_DEFAULT;
	t->weight = 1024;
	t->state  = THREAD_NEW;
	t->affinity_mask = ~0ull;
	t->pgtable = t->task->as ? t->task->as->pgtable : arch_pgtable_kernel();
	strlcpy(t->name, name ? name : "thread", sizeof(t->name));

	list_init(&t->runq_link);
	list_init(&t->task_link);
	list_init(&t->wait_link);
	list_init(&t->timer_link);

	arch_thread_init(t, entry, arg, t->kstack + RK_KSTACK_SIZE);

	unsigned long f = spin_lock_irqsave(&global_lock);
	list_add_tail(&t->task_link, &all_threads);
	t->task->nthreads++;
	gstats.threads_total++;
	gstats.threads_live++;
	spin_unlock_irqrestore(&global_lock, f);

	struct graph_node *n = rk_graph_node_create(GNODE_THREAD, t->name, NULL, t);
	if (n) {
		rk_graph_set_u64(n, "tid", t->tid);
		rk_graph_set_str(n, "class", sched_class_name(cls));
		rk_graph_set_u64(n, "prio", t->priority);
	}
	return t;
}

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg,
                             enum sched_class cls, u32 priority)
{
	return thread_create_in(&kernel_task, name, entry, arg, cls, priority);
}

void thread_start(struct thread *t)
{
	if (!t || t->state != THREAD_NEW)
		return;
	unsigned long f = spin_lock_irqsave(&rq_shared.lock);
	rq_enqueue(&rq_shared, t);
	spin_unlock_irqrestore(&rq_shared.lock, f);
}

void thread_exit(int code)
{
	struct thread *t = thread_current();
	if (!t) {
		pr_err("thread_exit with no current thread");
		arch_halt();
	}
	t->exit_code = code;

	unsigned long f = spin_lock_irqsave(&global_lock);
	t->state = THREAD_ZOMBIE;
	if (gstats.threads_live)
		gstats.threads_live--;
	if (t->task && t->task->nthreads)
		t->task->nthreads--;
	spin_unlock_irqrestore(&global_lock, f);

	pr_debug("thread %llu (%s) exited with %d",
	         (unsigned long long)t->tid, t->name, code);

	/* Never returns: the next schedule will not pick a zombie. */
	for (;;)
		__schedule(THREAD_ZOMBIE);
}

int thread_join(struct thread *t, int *code)
{
	if (!t)
		return RK_EINVAL;
	while (t->state != THREAD_ZOMBIE && t->state != THREAD_DEAD)
		sched_sleep_ms(1);
	if (code)
		*code = t->exit_code;
	return RK_OK;
}

void thread_destroy(struct thread *t)
{
	if (!t || t->state == THREAD_RUNNING)
		return;
	unsigned long f = spin_lock_irqsave(&global_lock);
	list_del(&t->task_link);
	spin_unlock_irqrestore(&global_lock, f);

	arch_thread_free(t);
	if (t->kstack)
		kfree((void *)t->kstack);
	t->state = THREAD_DEAD;
	kmem_cache_free(thread_cache, t);
}

struct thread *thread_by_id(rk_id_t tid)
{
	struct thread *t;
	unsigned long f = spin_lock_irqsave(&global_lock);
	struct thread *found = NULL;
	list_for_each_entry(t, &all_threads, task_link) {
		if (t->tid == tid) {
			found = t;
			break;
		}
	}
	spin_unlock_irqrestore(&global_lock, f);
	return found;
}

/* -------------------------------------------------------------- tasks */

struct task *task_create(const char *name, struct task *parent)
{
	struct task *t = kmem_cache_alloc(task_cache);
	if (!t)
		return NULL;
	memset(t, 0, sizeof(*t));

	t->pid = __atomic_add_fetch(&next_pid, 1, __ATOMIC_SEQ_CST) - 1;
	strlcpy(t->name, name ? name : "task", sizeof(t->name));
	t->parent = parent;
	list_init(&t->children);
	list_init(&t->sibling);
	list_init(&t->threads);
	spin_lock_init(&t->lock, "task");
	t->start_time_ns = rk_time_ns();

	t->as   = as_create();
	t->caps = capspace_create();
	if (!t->as || !t->caps) {
		if (t->as)   as_destroy(t->as);
		if (t->caps) capspace_destroy(t->caps);
		kmem_cache_free(task_cache, t);
		return NULL;
	}

	unsigned long f = spin_lock_irqsave(&global_lock);
	list_add_tail(&t->sibling, parent ? &parent->children : &all_tasks);
	spin_unlock_irqrestore(&global_lock, f);

	struct graph_node *n = rk_graph_node_create(GNODE_TASK, t->name, NULL, t);
	if (n) {
		rk_graph_set_u64(n, "pid", t->pid);
		t->graph_node = n->id;
	}
	pr_info("task %llu (%s) created", (unsigned long long)t->pid, t->name);
	return t;
}

void task_exit(struct task *t, int code)
{
	if (!t || t == &kernel_task)
		return;
	t->exit_code = code;
	t->exited = true;

	struct thread *th;
	unsigned long f = spin_lock_irqsave(&global_lock);
	list_for_each_entry(th, &all_threads, task_link)
		if (th->task == t)
			th->should_stop = true;
	spin_unlock_irqrestore(&global_lock, f);

	pr_info("task %llu (%s) exiting with %d",
	        (unsigned long long)t->pid, t->name, code);
}

struct task *task_by_id(rk_id_t pid)
{
	struct task *t;
	list_for_each_entry(t, &all_tasks, sibling)
		if (t->pid == pid)
			return t;
	return NULL;
}

/* ---------------------------------------------------------- policy knobs */

int sched_set_class(struct thread *t, enum sched_class cls, u32 prio)
{
	if (!t || (unsigned)cls >= SCHED_NCLASSES)
		return RK_EINVAL;
	struct runqueue *rq = &rq_shared;
	unsigned long f = spin_lock_irqsave(&rq->lock);
	bool queued = (t->state == THREAD_READY);
	if (queued)
		rq_dequeue(rq, t);
	t->sclass = cls;
	t->priority = t->base_priority = prio < RK_PRIO_LEVELS ? prio : RK_PRIO_DEFAULT;
	if (queued)
		rq_enqueue(rq, t);
	spin_unlock_irqrestore(&rq->lock, f);
	return RK_OK;
}

/* Admission control. A deadline class only means anything if the kernel
 * refuses work it cannot fit: accepting everything and missing deadlines is
 * indistinguishable from having no real-time support at all. */
int sched_set_deadline(struct thread *t, u64 period_ns, u64 budget_ns)
{
	if (!t || !period_ns || budget_ns > period_ns)
		return RK_EINVAL;

	u64 used_num = 0, used_den = 1;
	struct thread *o;
	unsigned long f = spin_lock_irqsave(&global_lock);
	list_for_each_entry(o, &all_threads, task_link) {
		if (o == t || !o->period_ns)
			continue;
		if (o->sclass != SCHED_REALTIME && o->sclass != SCHED_INFERENCE)
			continue;
		/* Sum of budget/period, scaled to avoid division. */
		used_num = used_num * o->period_ns + o->budget_ns * used_den;
		used_den = used_den * o->period_ns;
	}
	spin_unlock_irqrestore(&global_lock, f);

	/* Cap total deadline utilisation at 80% of one CPU per core, leaving
	 * headroom for interrupts and the rest of the system. */
	u64 want_num = used_num * period_ns + budget_ns * used_den;
	u64 want_den = used_den * period_ns;
	u64 cap_num = 8ull * arch_cpu_count();
	if (want_den && want_num * 10 > cap_num * want_den) {
		pr_warn("deadline admission refused for %s: %llu/%llu ns would overcommit",
		        t->name, (unsigned long long)budget_ns,
		        (unsigned long long)period_ns);
		return RK_EBUSY;
	}

	t->period_ns      = period_ns;
	t->budget_ns      = budget_ns;
	t->budget_left_ns = budget_ns;
	t->deadline_ns    = rk_time_ns() + period_ns;
	return RK_OK;
}

int sched_set_affinity(struct thread *t, u64 mask)
{
	if (!t || !mask)
		return RK_EINVAL;
	t->affinity_mask = mask;
	return RK_OK;
}

void sched_stats(struct sched_stats *out)
{
	*out = gstats;
	for (u32 c = 0; c < SCHED_NCLASSES; c++)
		out->runnable[c] = 0;
	struct thread *t;
	unsigned long f = spin_lock_irqsave(&global_lock);
	list_for_each_entry(t, &all_threads, task_link)
		if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
			out->runnable[t->sclass]++;
	spin_unlock_irqrestore(&global_lock, f);

	for (u32 c = 0; c < arch_cpu_count() && c < RK_MAX_CPUS; c++)
		out->idle_ns += cpu_state[c].idle_ns;
}

size_t sched_dump(char *buf, size_t cap)
{
	size_t n = 0;
	n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
	                      "%6s %-18s %-11s %5s %10s %8s %8s\n",
	                      "tid", "name", "class", "prio", "cpu_ns", "vcsw", "state");
	struct thread *t;
	unsigned long f = spin_lock_irqsave(&global_lock);
	list_for_each_entry(t, &all_threads, task_link)
		n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
		                      "%6llu %-18s %-11s %5u %10llu %8llu %8s\n",
		                      (unsigned long long)t->tid, t->name,
		                      sched_class_name(t->sclass), t->priority,
		                      (unsigned long long)t->runtime_ns,
		                      (unsigned long long)t->nvcsw,
		                      thread_state_name((enum thread_state)t->state));
	spin_unlock_irqrestore(&global_lock, f);
	return n;
}

/* ------------------------------------------------------------ wait queues */

void waitq_init(struct waitqueue *w, const char *name)
{
	list_init(&w->waiters);
	spin_lock_init(&w->lock, name);
	w->name = name;
}

void waitq_wait(struct waitqueue *w)
{
	struct thread *t = thread_current();
	if (!t) {
		/* Before the scheduler exists, a wait is a spin. Only early boot code
		 * can reach this, and only briefly. */
		arch_idle();
		return;
	}
	unsigned long f = spin_lock_irqsave(&w->lock);
	list_add_tail(&t->wait_link, &w->waiters);
	spin_unlock_irqrestore(&w->lock, f);
	sched_block();

	f = spin_lock_irqsave(&w->lock);
	if (list_linked(&t->wait_link))
		list_del(&t->wait_link);
	spin_unlock_irqrestore(&w->lock, f);
}

int waitq_wait_timeout(struct waitqueue *w, u64 ns)
{
	struct thread *t = thread_current();
	if (!t)
		return RK_EAGAIN;

	u64 deadline = rk_time_ns() + ns;
	unsigned long f = spin_lock_irqsave(&w->lock);
	list_add_tail(&t->wait_link, &w->waiters);
	spin_unlock_irqrestore(&w->lock, f);

	t->sleep_until_ns = deadline;
	__schedule(THREAD_SLEEPING);

	f = spin_lock_irqsave(&w->lock);
	bool still_queued = list_linked(&t->wait_link);
	if (still_queued)
		list_del(&t->wait_link);
	spin_unlock_irqrestore(&w->lock, f);

	return still_queued ? RK_ETIMEDOUT : RK_OK;
}

void waitq_wake_one(struct waitqueue *w)
{
	unsigned long f = spin_lock_irqsave(&w->lock);
	if (!list_empty(&w->waiters)) {
		struct thread *t = list_first_entry(&w->waiters, struct thread, wait_link);
		list_del(&t->wait_link);
		spin_unlock_irqrestore(&w->lock, f);
		sched_wake(t);
		return;
	}
	spin_unlock_irqrestore(&w->lock, f);
}

void waitq_wake_all(struct waitqueue *w)
{
	unsigned long f = spin_lock_irqsave(&w->lock);
	struct thread *t, *tmp;
	list_for_each_entry_safe(t, tmp, &w->waiters, wait_link) {
		list_del(&t->wait_link);
		sched_wake(t);
	}
	spin_unlock_irqrestore(&w->lock, f);
}

/* ------------------------------------------------------------------ init */

static void idle_entry(void *arg)
{
	struct cpu_state *cs = arg;
	for (;;) {
		u64 t0 = rk_time_ns();
		cs->idling = true;
		arch_idle();
		cs->idling = false;
		cs->idle_ns += rk_time_ns() - t0;
		sched_yield();
	}
}

/* An idle core is halted until its next tick, which is up to a whole
 * scheduling period away. That is a long time to leave a runnable thread
 * waiting while a core sits doing nothing, so poke one. Only one: waking
 * every idle core to race for a single thread is how a thundering herd
 * starts. */
static void kick_idle_cpu(u32 self)
{
	for (u32 c = 0; c < RK_MAX_CPUS; c++) {
		if (c == self || !cpu_state[c].online || !cpu_state[c].idling)
			continue;
		cpu_state[c].need_resched = true;
		arch_smp_send_ipi(c, RK_IPI_RESCHED);
		return;
	}
}

/* The far end of arch_smp_send_ipi, in portable code because what an IPI
 * means is a scheduler question, not an architecture one. */
void rk_ipi_handle(u32 kind)
{
	switch (kind) {
	case RK_IPI_RESCHED:
		this_cpu_state()->need_resched = true;
		break;
	case RK_IPI_TLB:
		arch_tlb_flush_all();
		break;
	case RK_IPI_HALT:
		arch_halt();
	default:
		break;
	}
}

/* Adopt the current execution context as this core's idle thread.
 *
 * The bootstrap path already has a stack and a register context, so it needs a
 * thread object rather than a new stack: it *becomes* the idle thread rather
 * than switching to one. Every core does this once, which is how a core that
 * has nothing to run has somewhere to be. */
static struct thread *adopt_as_idle(u32 cpu)
{
	struct thread *idle = kmem_cache_alloc(thread_cache);
	RK_ASSERT_MSG(idle, "cannot allocate the idle thread for cpu%u", cpu);

	memset(idle, 0, sizeof(*idle));
	idle->tid = 0;
	idle->task = &kernel_task;
	idle->sclass = SCHED_BATCH;
	idle->priority = RK_PRIO_LEVELS - 1;
	idle->weight = 1;
	idle->state = THREAD_RUNNING;
	idle->cpu = cpu;
	idle->pgtable = arch_pgtable_kernel();
	/* Pinned: an idle thread is the property of one core and must never be
	 * picked up by another. */
	idle->affinity_mask = 1ull << (cpu & 63);
	snprintf(idle->name, sizeof(idle->name), "idle/%u", cpu);
	list_init(&idle->runq_link);
	list_init(&idle->task_link);
	list_init(&idle->wait_link);
	list_init(&idle->timer_link);

	struct cpu_state *cs = &cpu_state[cpu];
	cs->idle = idle;
	cs->current = idle;
	cs->online = true;
	arch_set_current_thread(idle);
	return idle;
}

void sched_init(void)
{
	thread_cache = kmem_cache_create("thread", sizeof(struct thread), 64);
	task_cache   = kmem_cache_create("task", sizeof(struct task), 64);

	memset(&kernel_task, 0, sizeof(kernel_task));
	strlcpy(kernel_task.name, "kernel", sizeof(kernel_task.name));
	list_init(&kernel_task.children);
	list_init(&kernel_task.sibling);
	list_init(&kernel_task.threads);
	spin_lock_init(&kernel_task.lock, "kernel-task");
	kernel_task.as = as_kernel();
	kernel_task.caps = capspace_create();
	list_add_tail(&kernel_task.sibling, &all_tasks);

	spin_lock_init(&rq_shared.lock, "runqueue");
	for (u32 k = 0; k < SCHED_NCLASSES; k++)
		for (u32 p = 0; p < RK_PRIO_LEVELS; p++)
			list_init(&rq_shared.ready[k][p]);

	pr_info("scheduler ready: 4 classes, %u priority levels, shared run queue",
	        RK_PRIO_LEVELS);
}

void sched_start(void)
{
	struct cpu_state *cs = &cpu_state[0];
	adopt_as_idle(0);
	sched_ready = true;

	pr_info("scheduling started");
	idle_entry(cs);
	RK_UNREACHABLE();
}

/* Called by every application processor once it has its own GDT, IDT, per-CPU
 * block and local timer. It joins the same run queue the boot processor is
 * already serving, so work spreads with no balancing policy at all. */
void sched_start_secondary(u32 cpu)
{
	if (cpu >= RK_MAX_CPUS)
		arch_halt();

	struct cpu_state *cs = &cpu_state[cpu];
	adopt_as_idle(cpu);

	arch_irq_enable();
	idle_entry(cs);
	RK_UNREACHABLE();
}

u32 sched_cpus_online(void)
{
	u32 n = 0;
	for (u32 c = 0; c < RK_MAX_CPUS; c++)
		if (cpu_state[c].online)
			n++;
	return n;
}
