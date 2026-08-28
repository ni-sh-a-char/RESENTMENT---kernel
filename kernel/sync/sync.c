/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - sleeping synchronisation.
 *
 * Everything here blocks, so none of it is legal in interrupt context. The
 * assertions that enforce that are not paranoia: a mutex taken from an IRQ
 * handler deadlocks intermittently under load, which is the worst possible
 * failure mode to debug.
 */
#include <rk/sync.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/panic.h>
#include <rk/irq.h>
#include <rk/time.h>
#include <rk/string.h>

#undef RK_SUBSYS
#define RK_SUBSYS "sync"

#define ASSERT_SLEEPABLE(what) \
	RK_ASSERT_MSG(!rk_in_irq(), "%s from interrupt context", what)

/* ---------------------------------------------------------------- mutex */

void mutex_init(struct mutex *m, const char *name)
{
	m->owner = NULL;
	m->depth = 0;
	m->recursive = false;
	m->name = name;
	waitq_init(&m->wq, name);
	spin_lock_init(&m->lock, name);
}

void mutex_init_recursive(struct mutex *m, const char *name)
{
	mutex_init(m, name);
	m->recursive = true;
}

void mutex_lock(struct mutex *m)
{
	ASSERT_SLEEPABLE("mutex_lock");
	struct thread *self = thread_current();

	for (;;) {
		unsigned long f = spin_lock_irqsave(&m->lock);
		if (!m->owner) {
			m->owner = self;
			m->depth = 1;
			spin_unlock_irqrestore(&m->lock, f);
			return;
		}
		if (m->recursive && m->owner == self) {
			m->depth++;
			spin_unlock_irqrestore(&m->lock, f);
			return;
		}
		RK_ASSERT_MSG(m->owner != self || m->recursive,
		              "recursive acquisition of non-recursive mutex %s",
		              m->name ? m->name : "?");
		spin_unlock_irqrestore(&m->lock, f);
		waitq_wait(&m->wq);
	}
}

bool mutex_trylock(struct mutex *m)
{
	unsigned long f = spin_lock_irqsave(&m->lock);
	bool got = false;
	struct thread *self = thread_current();
	if (!m->owner) {
		m->owner = self;
		m->depth = 1;
		got = true;
	} else if (m->recursive && m->owner == self) {
		m->depth++;
		got = true;
	}
	spin_unlock_irqrestore(&m->lock, f);
	return got;
}

void mutex_unlock(struct mutex *m)
{
	unsigned long f = spin_lock_irqsave(&m->lock);
	if (m->depth > 1) {
		m->depth--;
		spin_unlock_irqrestore(&m->lock, f);
		return;
	}
	m->owner = NULL;
	m->depth = 0;
	spin_unlock_irqrestore(&m->lock, f);
	waitq_wake_one(&m->wq);
}

bool mutex_held(const struct mutex *m)
{
	return m->owner == thread_current();
}

/* ------------------------------------------------------------ semaphore */

void semaphore_init(struct semaphore *s, s64 initial, const char *name)
{
	s->count = initial;
	s->name = name;
	waitq_init(&s->wq, name);
	spin_lock_init(&s->lock, name);
}

void semaphore_wait(struct semaphore *s)
{
	ASSERT_SLEEPABLE("semaphore_wait");
	for (;;) {
		unsigned long f = spin_lock_irqsave(&s->lock);
		if (s->count > 0) {
			s->count--;
			spin_unlock_irqrestore(&s->lock, f);
			return;
		}
		spin_unlock_irqrestore(&s->lock, f);
		waitq_wait(&s->wq);
	}
}

int semaphore_wait_timeout(struct semaphore *s, u64 ns)
{
	ASSERT_SLEEPABLE("semaphore_wait_timeout");
	u64 deadline = rk_time_ns() + ns;
	for (;;) {
		unsigned long f = spin_lock_irqsave(&s->lock);
		if (s->count > 0) {
			s->count--;
			spin_unlock_irqrestore(&s->lock, f);
			return RK_OK;
		}
		spin_unlock_irqrestore(&s->lock, f);

		u64 now = rk_time_ns();
		if (now >= deadline)
			return RK_ETIMEDOUT;
		if (waitq_wait_timeout(&s->wq, deadline - now) == RK_ETIMEDOUT)
			return RK_ETIMEDOUT;
	}
}

bool semaphore_trywait(struct semaphore *s)
{
	unsigned long f = spin_lock_irqsave(&s->lock);
	bool got = s->count > 0;
	if (got)
		s->count--;
	spin_unlock_irqrestore(&s->lock, f);
	return got;
}

/* Safe from interrupt context: it never sleeps and the wake is deferred. */
void semaphore_post(struct semaphore *s)
{
	unsigned long f = spin_lock_irqsave(&s->lock);
	s->count++;
	spin_unlock_irqrestore(&s->lock, f);
	waitq_wake_one(&s->wq);
}

/* --------------------------------------------------------------- rwlock */

void rwlock_init(struct rwlock *l, const char *name)
{
	l->readers = 0;
	l->writer = false;
	l->waiting_writers = 0;
	l->name = name;
	spin_lock_init(&l->lock, name);
	waitq_init(&l->rq, name);
	waitq_init(&l->wq, name);
}

/* Writer-preferring: a new reader waits if a writer is queued. Without this,
 * a steady stream of graph queries would starve a graph update forever. */
void read_lock(struct rwlock *l)
{
	ASSERT_SLEEPABLE("read_lock");
	for (;;) {
		unsigned long f = spin_lock_irqsave(&l->lock);
		if (!l->writer && l->waiting_writers == 0) {
			l->readers++;
			spin_unlock_irqrestore(&l->lock, f);
			return;
		}
		spin_unlock_irqrestore(&l->lock, f);
		waitq_wait(&l->rq);
	}
}

void read_unlock(struct rwlock *l)
{
	unsigned long f = spin_lock_irqsave(&l->lock);
	if (l->readers)
		l->readers--;
	bool last = (l->readers == 0);
	spin_unlock_irqrestore(&l->lock, f);
	if (last)
		waitq_wake_one(&l->wq);
}

void write_lock(struct rwlock *l)
{
	ASSERT_SLEEPABLE("write_lock");
	unsigned long f = spin_lock_irqsave(&l->lock);
	l->waiting_writers++;
	spin_unlock_irqrestore(&l->lock, f);

	for (;;) {
		f = spin_lock_irqsave(&l->lock);
		if (!l->writer && l->readers == 0) {
			l->writer = true;
			l->waiting_writers--;
			spin_unlock_irqrestore(&l->lock, f);
			return;
		}
		spin_unlock_irqrestore(&l->lock, f);
		waitq_wait(&l->wq);
	}
}

void write_unlock(struct rwlock *l)
{
	unsigned long f = spin_lock_irqsave(&l->lock);
	l->writer = false;
	bool writers_waiting = l->waiting_writers > 0;
	spin_unlock_irqrestore(&l->lock, f);

	if (writers_waiting)
		waitq_wake_one(&l->wq);
	else
		waitq_wake_all(&l->rq);
}

/* -------------------------------------------------------------- condvar */

void condvar_init(struct condvar *c, const char *name)
{
	c->name = name;
	waitq_init(&c->wq, name);
}

void condvar_wait(struct condvar *c, struct mutex *m)
{
	ASSERT_SLEEPABLE("condvar_wait");
	mutex_unlock(m);
	waitq_wait(&c->wq);
	mutex_lock(m);
}

int condvar_wait_timeout(struct condvar *c, struct mutex *m, u64 ns)
{
	ASSERT_SLEEPABLE("condvar_wait_timeout");
	mutex_unlock(m);
	int r = waitq_wait_timeout(&c->wq, ns);
	mutex_lock(m);
	return r;
}

void condvar_signal(struct condvar *c)    { waitq_wake_one(&c->wq); }
void condvar_broadcast(struct condvar *c) { waitq_wake_all(&c->wq); }

/* ----------------------------------------------------------------- once */

#define ONCE_IDLE 0
#define ONCE_BUSY 1
#define ONCE_DONE 2

void rk_once(struct once *o, void (*fn)(void))
{
	u32 expected = ONCE_IDLE;
	if (atomic32_cas(&o->state, &expected, ONCE_BUSY)) {
		fn();
		atomic32_store(&o->state, ONCE_DONE);
		return;
	}
	/* Another CPU is running it. Spin rather than sleep: initialisers are
	 * short by construction and this can be called before the scheduler. */
	while (atomic32_load(&o->state) != ONCE_DONE)
		arch_cpu_relax();
}

/* ----------------------------------------------------------- completion */

void completion_init(struct completion *c)
{
	c->done = false;
	waitq_init(&c->wq, "completion");
	spin_lock_init(&c->lock, "completion");
}

/* The done flag is checked under the lock before sleeping, which is what makes
 * a completion survive being signalled before anyone waits on it. */
void completion_wait(struct completion *c)
{
	ASSERT_SLEEPABLE("completion_wait");
	for (;;) {
		unsigned long f = spin_lock_irqsave(&c->lock);
		if (c->done) {
			spin_unlock_irqrestore(&c->lock, f);
			return;
		}
		spin_unlock_irqrestore(&c->lock, f);
		waitq_wait(&c->wq);
	}
}

int completion_wait_timeout(struct completion *c, u64 ns)
{
	ASSERT_SLEEPABLE("completion_wait_timeout");
	u64 deadline = rk_time_ns() + ns;
	for (;;) {
		unsigned long f = spin_lock_irqsave(&c->lock);
		if (c->done) {
			spin_unlock_irqrestore(&c->lock, f);
			return RK_OK;
		}
		spin_unlock_irqrestore(&c->lock, f);

		u64 now = rk_time_ns();
		if (now >= deadline)
			return RK_ETIMEDOUT;
		waitq_wait_timeout(&c->wq, deadline - now);
	}
}

void completion_done(struct completion *c)
{
	unsigned long f = spin_lock_irqsave(&c->lock);
	c->done = true;
	spin_unlock_irqrestore(&c->lock, f);
	waitq_wake_all(&c->wq);
}
