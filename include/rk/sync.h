/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - sleeping synchronisation primitives.
 *
 * Spinlocks live in rk/spinlock.h and never sleep. Everything here may block,
 * so none of it is legal in interrupt context.
 */
#pragma once

#include <rk/types.h>
#include <rk/sched.h>
#include <rk/spinlock.h>

struct mutex {
	struct thread   *owner;
	struct waitqueue wq;
	spinlock_t       lock;
	u32              depth;      /* recursive acquisition by the owner */
	bool             recursive;
	const char      *name;
};

/* Statically initialised so that a lock used before its subsystem's init
 * function runs still behaves. An uninitialised waitqueue has NULL list heads,
 * which makes the first unlock dereference NULL - a crash a long way from the
 * ordering mistake that caused it. */
#define WAITQUEUE_INIT(v, nm) {                          \
	.waiters = { &(v).waiters, &(v).waiters },            \
	.lock = SPINLOCK_INIT(nm), .name = nm }

#define DEFINE_MUTEX(v) struct mutex v = {               \
	.owner = NULL,                                       \
	.wq = WAITQUEUE_INIT((v).wq, #v),                    \
	.lock = SPINLOCK_INIT(#v),                           \
	.depth = 0, .recursive = false, .name = #v }

void mutex_init(struct mutex *m, const char *name);
void mutex_init_recursive(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_held(const struct mutex *m);

struct semaphore {
	s64              count;
	struct waitqueue wq;
	spinlock_t       lock;
	const char      *name;
};

void semaphore_init(struct semaphore *s, s64 initial, const char *name);
void semaphore_wait(struct semaphore *s);
int  semaphore_wait_timeout(struct semaphore *s, u64 ns);
bool semaphore_trywait(struct semaphore *s);
void semaphore_post(struct semaphore *s);

/* Writer-preferring: readers are cheap and frequent (graph queries, VFS
 * lookups) so a reader-preferring lock would starve the writer forever. */
struct rwlock {
	spinlock_t       lock;
	s32              readers;
	bool             writer;
	u32              waiting_writers;
	struct waitqueue rq, wq;
	const char      *name;
};

void rwlock_init(struct rwlock *l, const char *name);
void read_lock(struct rwlock *l);
void read_unlock(struct rwlock *l);
void write_lock(struct rwlock *l);
void write_unlock(struct rwlock *l);

struct condvar {
	struct waitqueue wq;
	const char      *name;
};

void condvar_init(struct condvar *c, const char *name);
void condvar_wait(struct condvar *c, struct mutex *m);
int  condvar_wait_timeout(struct condvar *c, struct mutex *m, u64 ns);
void condvar_signal(struct condvar *c);
void condvar_broadcast(struct condvar *c);

/* One-time initialisation that is safe from many CPUs at once. */
struct once { atomic32_t state; };
#define ONCE_INIT { ATOMIC_INIT(0) }
void rk_once(struct once *o, void (*fn)(void));

/* A completion is the "wait for this one thing to finish" case, which a
 * semaphore models badly because it must survive post-before-wait. */
struct completion {
	bool             done;
	struct waitqueue wq;
	spinlock_t       lock;
};
void completion_init(struct completion *c);
void completion_wait(struct completion *c);
int  completion_wait_timeout(struct completion *c, u64 ns);
void completion_done(struct completion *c);
