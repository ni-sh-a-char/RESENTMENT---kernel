/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - tasks, threads and scheduling.
 *
 * Four scheduling classes, checked in this order:
 *
 *   REALTIME    earliest-deadline-first, admission controlled
 *   INFERENCE   deadline-aware class for AI work; a token-generation loop is
 *               neither interactive nor batch, it is a soft-real-time stream
 *               with a target rate, and treating it as batch is why inference
 *               stutters on general purpose systems
 *   INTERACTIVE multi-level feedback queue with sleep-credit boosting
 *   BATCH       weighted fair share of what is left
 *
 * Every context switch is a graph event, so a whole scheduling trace can be
 * replayed deterministically (see rk/graph.h).
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/spinlock.h>
#include <rk/arch.h>

#define RK_TASK_NAME_MAX 32
#define RK_PRIO_LEVELS   32
#define RK_PRIO_DEFAULT  16
#define RK_KSTACK_SIZE   (16 * 1024)

enum thread_state {
	THREAD_NEW = 0,
	THREAD_READY,
	THREAD_RUNNING,
	THREAD_BLOCKED,
	THREAD_SLEEPING,
	THREAD_ZOMBIE,
	THREAD_DEAD,
};

enum sched_class {
	SCHED_REALTIME = 0,
	SCHED_INFERENCE,
	SCHED_INTERACTIVE,
	SCHED_BATCH,
	SCHED_NCLASSES
};

struct task;
struct capspace;
struct address_space;

struct thread {
	/* Hot fields first: the context switch path touches only these. */
	void             *sp;            /* saved stack pointer */
	pgtable_t         pgtable;
	u32               cpu;
	volatile u32      state;

	rk_id_t           tid;
	char              name[RK_TASK_NAME_MAX];
	struct task      *task;

	enum sched_class  sclass;
	u32               priority;      /* 0 = highest */
	u32               base_priority;
	u64               deadline_ns;   /* REALTIME and INFERENCE */
	u64               period_ns;     /* 0 = aperiodic */
	u64               budget_ns;     /* per-period execution budget */
	u64               budget_left_ns;

	u64               vruntime;      /* BATCH fair-share virtual time */
	u32               weight;

	u64               slice_ns;
	u64               runtime_ns;    /* total CPU consumed */
	u64               last_start_ns;
	u64               wakeup_ns;
	u64               nvcsw, nivcsw; /* voluntary / involuntary switches */

	struct list_head  runq_link;
	struct list_head  task_link;
	struct list_head  wait_link;
	struct list_head  timer_link;
	u64               sleep_until_ns;

	vaddr_t           kstack;
	size_t            kstack_size;
	void             *fpu_state;

	u64               affinity_mask;
	int               exit_code;
	volatile bool     should_stop;
	bool              in_kernel;

	/* Deterministic replay: a monotonically increasing per-thread event
	 * counter that the graph uses to order everything this thread did. */
	u64               event_seq;
};

/* A task is an address space, a capability space and a set of threads: the
 * unit of isolation. RESENTMENT has no ambient authority, so a task with an
 * empty capspace can do literally nothing but compute and exit. */
struct task {
	rk_id_t               pid;
	char                  name[RK_TASK_NAME_MAX];
	struct address_space *as;
	struct capspace      *caps;
	struct task          *parent;
	struct list_head      children;
	struct list_head      sibling;
	struct list_head      threads;
	u32                   nthreads;
	spinlock_t            lock;
	int                   exit_code;
	volatile bool         exited;
	u64                   start_time_ns;
	u64                   cpu_time_ns;
	rk_id_t               graph_node;   /* identity in the runtime graph */
};

/* ------------------------------------------------------------------- API */

void   sched_init(void);
void   sched_start(void) __noreturn;   /* becomes the idle thread */
bool   sched_active(void);             /* false before sched_init */
/* Entry point for an application processor, once it has its own per-CPU block,
 * descriptor tables and local timer. Never returns. */
void   sched_start_secondary(u32 cpu) __noreturn;
u32    sched_cpus_online(void);
/* Called by the architecture thread trampoline as the first thing a new thread
 * does, and by the scheduler itself after every switch. */
void   sched_finish_switch(void);

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg,
                             enum sched_class cls, u32 priority);
struct thread *thread_create_in(struct task *t, const char *name,
                                void (*entry)(void *), void *arg,
                                enum sched_class cls, u32 priority);
void   thread_start(struct thread *t);
void   thread_exit(int code) __noreturn;
int    thread_join(struct thread *t, int *code);
void   thread_destroy(struct thread *t);
struct thread *thread_current(void);
struct thread *thread_by_id(rk_id_t tid);

struct task *task_create(const char *name, struct task *parent);
void   task_exit(struct task *t, int code);
struct task *task_current(void);
struct task *task_by_id(rk_id_t pid);
struct task *task_kernel(void);

void   sched_yield(void);
void   sched_sleep_ns(u64 ns);
void   sched_sleep_ms(u64 ms);
void   sched_block(void);                 /* caller is already on a wait queue */
void   sched_wake(struct thread *t);
void   sched_tick(void);                  /* from the timer interrupt */
void   sched_preempt_disable(void);
void   sched_preempt_enable(void);

int    sched_set_class(struct thread *t, enum sched_class cls, u32 prio);
int    sched_set_deadline(struct thread *t, u64 period_ns, u64 budget_ns);
int    sched_set_affinity(struct thread *t, u64 mask);

const char *sched_class_name(enum sched_class c);
const char *thread_state_name(enum thread_state s);

struct sched_stats {
	u64 switches, preemptions, migrations, idle_ns;
	u64 runnable[SCHED_NCLASSES];
	u64 deadline_misses;
	u64 threads_total, threads_live;
};
void sched_stats(struct sched_stats *out);

/* ------------------------------------------------------------ wait queues */

struct waitqueue {
	struct list_head waiters;
	spinlock_t       lock;
	const char      *name;
};

void waitq_init(struct waitqueue *w, const char *name);
void waitq_wait(struct waitqueue *w);
int  waitq_wait_timeout(struct waitqueue *w, u64 ns);
void waitq_wake_one(struct waitqueue *w);
void waitq_wake_all(struct waitqueue *w);

#define waitq_wait_event(wq, cond)         \
	do {                                   \
		while (!(cond))                    \
			waitq_wait(wq);                \
	} while (0)
