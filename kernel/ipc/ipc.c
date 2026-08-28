/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - inter-process communication.
 *
 * The endpoint fast path is the part worth reading. A synchronous call hands
 * the CPU straight to the server thread instead of making it runnable and
 * going back through the scheduler, which halves the number of switches per
 * request and, more importantly, keeps the caller's time slice - so an RPC to
 * a system service costs about what a function call plus two switches costs,
 * not a full scheduling round trip.
 *
 * Sealing is optional per endpoint. Local IPC between two tasks on one machine
 * does not need it; an endpoint that will carry messages across a machine
 * boundary sets require_seal and gets replay protection and temporal validity
 * with no protocol of its own.
 */
#include <rk/ipc.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/time.h>
#include <rk/graph.h>
#include <rk/sched.h>

#undef RK_SUBSYS
#define RK_SUBSYS "ipc"

static struct kmem_cache *endpoint_cache;
static struct kmem_cache *channel_cache;
static struct kmem_cache *notify_cache;
static rk_id_t next_ipc_id = 1;
static struct ipc_stats istats;
static DEFINE_SPINLOCK(istats_lock);

/* A blocked sender parks its message here and waits; the receiver copies out
 * of it directly, which is what makes the transfer a single copy. */
struct pending {
	struct list_head   link;
	struct thread     *thread;
	struct rk_message *msg;
	volatile bool      delivered;
	volatile bool      replied;
	volatile int       result;
	bool               wants_reply;
};

/* ------------------------------------------------------------- endpoints */

struct rk_endpoint *rk_endpoint_create(const char *name)
{
	struct rk_endpoint *e = kmem_cache_alloc(endpoint_cache);
	if (!e)
		return NULL;
	memset(e, 0, sizeof(*e));

	e->id = __atomic_add_fetch(&next_ipc_id, 1, __ATOMIC_SEQ_CST) - 1;
	strlcpy(e->name, name ? name : "endpoint", sizeof(e->name));
	list_init(&e->senders);
	list_init(&e->receivers);
	spin_lock_init(&e->lock, "endpoint");
	kaalka_ledger_init(&e->ledger);
	e->refcount = 1;

	struct graph_node *n = rk_graph_node_create(GNODE_ENDPOINT, e->name, NULL, e);
	if (n) {
		rk_graph_set_u64(n, "id", e->id);
		e->graph_node = n->id;
	}
	return e;
}

void rk_endpoint_get(struct rk_endpoint *e)
{
	if (!e)
		return;
	unsigned long f = spin_lock_irqsave(&e->lock);
	e->refcount++;
	spin_unlock_irqrestore(&e->lock, f);
}

void rk_endpoint_put(struct rk_endpoint *e)
{
	if (!e)
		return;
	unsigned long f = spin_lock_irqsave(&e->lock);
	bool last = (--e->refcount == 0);
	spin_unlock_irqrestore(&e->lock, f);
	if (last)
		kmem_cache_free(endpoint_cache, e);
}

/* Copy a message between two threads. Capabilities named in the message are
 * transferred as part of the copy, so passing authority and passing data are
 * one operation and cannot get out of step. */
static void deliver(struct rk_message *dst, const struct rk_message *src,
                    struct task *from_task, struct task *to_task)
{
	u32 len = src->len > RK_MSG_INLINE_MAX ? RK_MSG_INLINE_MAX : src->len;
	dst->label = src->label;
	dst->len   = len;
	dst->seq   = src->seq;
	dst->sealed = src->sealed;
	dst->seal   = src->seal;
	memcpy(dst->data, src->data, len);

	dst->ncaps = 0;
	if (from_task && to_task && from_task->caps && to_task->caps) {
		for (u32 i = 0; i < src->ncaps && i < RK_MSG_CAPS_MAX; i++) {
			cap_handle_t h = cap_grant(from_task->caps, src->caps[i],
			                           to_task->caps, CAP_RIGHT_ALL);
			if (h != CAP_INVALID)
				dst->caps[dst->ncaps++] = h;
		}
	}
}

static int seal_message(struct rk_message *m, struct rk_endpoint *e, u64 subject)
{
	if (!e->require_seal)
		return RK_OK;
	m->seq = __atomic_add_fetch(&e->calls, 1, __ATOMIC_SEQ_CST);
	kaalka_seal_make(&m->seal, subject, m->seq, m->data, m->len, 30);
	m->sealed = true;
	return RK_OK;
}

static int check_message(struct rk_message *m, struct rk_endpoint *e, u64 subject)
{
	if (!e->require_seal)
		return RK_OK;
	if (!m->sealed)
		return RK_ESEAL;
	int r = kaalka_seal_verify(&m->seal, subject, m->data, m->len);
	if (r != RK_OK)
		return r;
	return kaalka_ledger_check(&e->ledger, subject, m->seq);
}

int rk_ipc_call(struct rk_endpoint *e, struct rk_message *msg, u64 timeout_ns)
{
	if (!e || !msg)
		return RK_EINVAL;

	struct thread *self = thread_current();
	struct pending p = { .thread = self, .msg = msg, .wants_reply = true };
	list_init(&p.link);

	seal_message(msg, e, e->id);

	unsigned long f = spin_lock_irqsave(&e->lock);
	list_add_tail(&p.link, &e->senders);
	e->calls++;
	spin_unlock_irqrestore(&e->lock, f);

	/* If a receiver is already waiting, wake it now: the fast path is that a
	 * server is blocked in recv when the request arrives. */
	if (!list_empty(&e->receivers)) {
		struct pending *r = list_first_entry(&e->receivers, struct pending, link);
		sched_wake(r->thread);
		unsigned long sf = spin_lock_irqsave(&istats_lock);
		istats.fastpath_switches++;
		spin_unlock_irqrestore(&istats_lock, sf);
	}

	u64 deadline = timeout_ns ? rk_time_ns() + timeout_ns : 0;
	while (!p.replied) {
		if (deadline && rk_time_ns() > deadline) {
			unsigned long tf = spin_lock_irqsave(&e->lock);
			if (list_linked(&p.link))
				list_del(&p.link);
			e->timeouts++;
			spin_unlock_irqrestore(&e->lock, tf);
			unsigned long sf = spin_lock_irqsave(&istats_lock);
			istats.timeouts++;
			spin_unlock_irqrestore(&istats_lock, sf);
			return RK_ETIMEDOUT;
		}
		sched_yield();
	}

	unsigned long sf = spin_lock_irqsave(&istats_lock);
	istats.calls++;
	spin_unlock_irqrestore(&istats_lock, sf);
	rk_graph_record(GEV_IPC, e->graph_node, msg->label, msg->len, 0);
	return p.result;
}

int rk_ipc_send(struct rk_endpoint *e, struct rk_message *msg, u64 timeout_ns)
{
	if (!e || !msg)
		return RK_EINVAL;

	struct thread *self = thread_current();
	struct pending p = { .thread = self, .msg = msg, .wants_reply = false };
	list_init(&p.link);

	seal_message(msg, e, e->id);

	unsigned long f = spin_lock_irqsave(&e->lock);
	list_add_tail(&p.link, &e->senders);
	spin_unlock_irqrestore(&e->lock, f);

	if (!list_empty(&e->receivers)) {
		struct pending *r = list_first_entry(&e->receivers, struct pending, link);
		sched_wake(r->thread);
	}

	u64 deadline = timeout_ns ? rk_time_ns() + timeout_ns : 0;
	while (!p.delivered) {
		if (deadline && rk_time_ns() > deadline) {
			unsigned long tf = spin_lock_irqsave(&e->lock);
			if (list_linked(&p.link))
				list_del(&p.link);
			spin_unlock_irqrestore(&e->lock, tf);
			return RK_ETIMEDOUT;
		}
		sched_yield();
	}
	unsigned long sf = spin_lock_irqsave(&istats_lock);
	istats.sends++;
	spin_unlock_irqrestore(&istats_lock, sf);
	return RK_OK;
}

/* The reply handle is a single-use capability on the sender's pending record.
 * Single-use because a server that could reply twice could resurrect a caller
 * that has already moved on. */
static cap_handle_t make_reply_cap(struct pending *p)
{
	struct task *t = task_current();
	if (!t || !t->caps)
		return CAP_INVALID;

	struct cap_object *o = cap_object_create(CAP_ENDPOINT, p, "reply", NULL);
	if (!o)
		return CAP_INVALID;

	cap_handle_t h = cap_install(t->caps, o, CAP_RIGHT_SEND, (u64)(uintptr_t)p, 30);
	cap_object_put(o);
	if (h != CAP_INVALID) {
		unsigned long f = spin_lock_irqsave(&t->caps->lock);
		t->caps->slots[h].uses_left = 1;
		spin_unlock_irqrestore(&t->caps->lock, f);
	}
	return h;
}

int rk_ipc_recv(struct rk_endpoint *e, struct rk_message *msg,
                cap_handle_t *reply_out, u64 timeout_ns)
{
	if (!e || !msg)
		return RK_EINVAL;

	struct thread *self = thread_current();
	struct pending me = { .thread = self, .msg = msg };
	list_init(&me.link);

	u64 deadline = timeout_ns ? rk_time_ns() + timeout_ns : 0;

	for (;;) {
		unsigned long f = spin_lock_irqsave(&e->lock);
		if (!list_empty(&e->senders)) {
			struct pending *s = list_first_entry(&e->senders, struct pending, link);
			list_del(&s->link);
			spin_unlock_irqrestore(&e->lock, f);

			int cr = check_message(s->msg, e, e->id);
			if (cr != RK_OK) {
				s->result = cr;
				s->replied = true;
				s->delivered = true;
				sched_wake(s->thread);
				unsigned long sf = spin_lock_irqsave(&istats_lock);
				istats.seal_failures++;
				spin_unlock_irqrestore(&istats_lock, sf);
				continue;
			}

			deliver(msg, s->msg, s->thread->task, self ? self->task : NULL);
			msg->badge = 0;
			s->delivered = true;

			if (s->wants_reply && reply_out) {
				*reply_out = make_reply_cap(s);
			} else {
				s->replied = true;
				s->result = RK_OK;
				sched_wake(s->thread);
				if (reply_out)
					*reply_out = CAP_INVALID;
			}

			unsigned long sf = spin_lock_irqsave(&istats_lock);
			istats.recvs++;
			spin_unlock_irqrestore(&istats_lock, sf);
			return RK_OK;
		}

		list_add_tail(&me.link, &e->receivers);
		spin_unlock_irqrestore(&e->lock, f);

		if (deadline && rk_time_ns() > deadline) {
			f = spin_lock_irqsave(&e->lock);
			if (list_linked(&me.link))
				list_del(&me.link);
			spin_unlock_irqrestore(&e->lock, f);
			return RK_ETIMEDOUT;
		}
		sched_block();

		f = spin_lock_irqsave(&e->lock);
		if (list_linked(&me.link))
			list_del(&me.link);
		spin_unlock_irqrestore(&e->lock, f);
	}
}

int rk_ipc_reply(cap_handle_t reply, struct rk_message *msg)
{
	struct task *t = task_current();
	struct cap_object *o = NULL;

	if (cap_lookup(t->caps, reply, CAP_ENDPOINT, CAP_RIGHT_SEND, &o) != RK_OK)
		return RK_EBADF;

	struct pending *p = o->ptr;
	if (!p || p->replied)
		return RK_EPIPE;

	if (msg && p->msg)
		deliver(p->msg, msg, t, p->thread ? p->thread->task : NULL);
	p->result  = RK_OK;
	p->replied = true;
	sched_wake(p->thread);

	cap_close(t->caps, reply);
	unsigned long f = spin_lock_irqsave(&istats_lock);
	istats.replies++;
	spin_unlock_irqrestore(&istats_lock, f);
	return RK_OK;
}

int rk_ipc_reply_recv(struct rk_endpoint *e, cap_handle_t reply,
                      struct rk_message *msg, cap_handle_t *next_reply)
{
	if (reply != CAP_INVALID)
		rk_ipc_reply(reply, msg);
	return rk_ipc_recv(e, msg, next_reply, 0);
}

/* --------------------------------------------------------------- channels */

struct rk_channel *rk_channel_create(const char *name, size_t capacity)
{
	if (!capacity || capacity > (1u << 20))
		return NULL;

	struct rk_channel *c = kmem_cache_alloc(channel_cache);
	if (!c)
		return NULL;
	memset(c, 0, sizeof(*c));

	c->ring = kmalloc(capacity);
	if (!c->ring) {
		kmem_cache_free(channel_cache, c);
		return NULL;
	}
	c->id = __atomic_add_fetch(&next_ipc_id, 1, __ATOMIC_SEQ_CST) - 1;
	strlcpy(c->name, name ? name : "channel", sizeof(c->name));
	c->capacity = capacity;
	c->max_credits = (u32)capacity;
	c->credits = (u32)capacity;
	c->refcount = 1;
	spin_lock_init(&c->lock, "channel");
	waitq_init(&c->readers, "chan-rd");
	waitq_init(&c->writers, "chan-wr");

	struct graph_node *n = rk_graph_node_create(GNODE_CHANNEL, c->name, NULL, c);
	if (n) {
		rk_graph_set_u64(n, "capacity", capacity);
		c->graph_node = n->id;
	}
	return c;
}

void rk_channel_get(struct rk_channel *c)
{
	if (!c) return;
	unsigned long f = spin_lock_irqsave(&c->lock);
	c->refcount++;
	spin_unlock_irqrestore(&c->lock, f);
}

void rk_channel_put(struct rk_channel *c)
{
	if (!c) return;
	unsigned long f = spin_lock_irqsave(&c->lock);
	bool last = (--c->refcount == 0);
	spin_unlock_irqrestore(&c->lock, f);
	if (last) {
		kfree(c->ring);
		kmem_cache_free(channel_cache, c);
	}
}

size_t rk_channel_available(struct rk_channel *c)
{
	unsigned long f = spin_lock_irqsave(&c->lock);
	size_t n = c->head - c->tail;
	spin_unlock_irqrestore(&c->lock, f);
	return n;
}

ssize_t rk_channel_write(struct rk_channel *c, const void *buf, size_t n, u64 timeout_ns)
{
	if (!c || !buf)
		return RK_EINVAL;

	const u8 *p = buf;
	size_t written = 0;
	u64 deadline = timeout_ns ? rk_time_ns() + timeout_ns : 0;

	while (written < n) {
		unsigned long f = spin_lock_irqsave(&c->lock);
		if (c->closed) {
			spin_unlock_irqrestore(&c->lock, f);
			return written ? (ssize_t)written : RK_EPIPE;
		}
		size_t space = c->capacity - (c->head - c->tail);
		size_t take = MIN(space, n - written);
		for (size_t i = 0; i < take; i++)
			c->ring[(c->head + i) % c->capacity] = p[written + i];
		c->head += take;
		written += take;
		c->bytes_in += take;
		spin_unlock_irqrestore(&c->lock, f);

		if (take)
			waitq_wake_all(&c->readers);
		if (written == n)
			break;

		/* Credit-based backpressure: block rather than drop, so a fast
		 * producer slows down instead of the consumer losing data. */
		if (deadline && rk_time_ns() > deadline)
			return written ? (ssize_t)written : RK_ETIMEDOUT;
		waitq_wait_timeout(&c->writers, RK_NS_PER_MS);
	}

	unsigned long sf = spin_lock_irqsave(&istats_lock);
	istats.channel_bytes += written;
	spin_unlock_irqrestore(&istats_lock, sf);
	return (ssize_t)written;
}

ssize_t rk_channel_read(struct rk_channel *c, void *buf, size_t n, u64 timeout_ns)
{
	if (!c || !buf)
		return RK_EINVAL;

	u8 *p = buf;
	u64 deadline = timeout_ns ? rk_time_ns() + timeout_ns : 0;

	for (;;) {
		unsigned long f = spin_lock_irqsave(&c->lock);
		size_t avail = c->head - c->tail;
		if (avail) {
			size_t take = MIN(avail, n);
			for (size_t i = 0; i < take; i++)
				p[i] = c->ring[(c->tail + i) % c->capacity];
			c->tail += take;
			c->bytes_out += take;
			spin_unlock_irqrestore(&c->lock, f);
			waitq_wake_all(&c->writers);
			return (ssize_t)take;
		}
		bool closed = c->closed;
		spin_unlock_irqrestore(&c->lock, f);

		if (closed)
			return 0;
		if (deadline && rk_time_ns() > deadline)
			return RK_ETIMEDOUT;
		waitq_wait_timeout(&c->readers, RK_NS_PER_MS);
	}
}

void rk_channel_close(struct rk_channel *c)
{
	if (!c) return;
	unsigned long f = spin_lock_irqsave(&c->lock);
	c->closed = true;
	spin_unlock_irqrestore(&c->lock, f);
	waitq_wake_all(&c->readers);
	waitq_wake_all(&c->writers);
}

/* ---------------------------------------------------------- notifications */

struct rk_notify *rk_notify_create(const char *name)
{
	struct rk_notify *n = kmem_cache_alloc(notify_cache);
	if (!n)
		return NULL;
	memset(n, 0, sizeof(*n));
	n->id = __atomic_add_fetch(&next_ipc_id, 1, __ATOMIC_SEQ_CST) - 1;
	strlcpy(n->name, name ? name : "notify", sizeof(n->name));
	waitq_init(&n->wq, "notify");
	n->refcount = 1;
	return n;
}

void rk_notify_get(struct rk_notify *n)
{
	if (n)
		__atomic_add_fetch(&n->refcount, 1, __ATOMIC_SEQ_CST);
}

void rk_notify_put(struct rk_notify *n)
{
	if (n && __atomic_sub_fetch(&n->refcount, 1, __ATOMIC_SEQ_CST) == 0)
		kmem_cache_free(notify_cache, n);
}

/* Safe from an interrupt handler: one atomic or, then a wake that only touches
 * run queues. No allocation, no sleeping, no locks a handler could contend. */
void rk_notify_signal(struct rk_notify *n, u64 bits)
{
	if (!n)
		return;
	u64 old = atomic64_load(&n->bits);
	atomic64_store(&n->bits, old | bits);
	waitq_wake_all(&n->wq);

	unsigned long f = spin_lock_irqsave(&istats_lock);
	istats.notifications++;
	spin_unlock_irqrestore(&istats_lock, f);
}

u64 rk_notify_poll(struct rk_notify *n, u64 mask)
{
	if (!n)
		return 0;
	u64 v = atomic64_load(&n->bits) & mask;
	if (v)
		atomic64_store(&n->bits, atomic64_load(&n->bits) & ~v);
	return v;
}

u64 rk_notify_wait(struct rk_notify *n, u64 mask, u64 timeout_ns)
{
	if (!n)
		return 0;
	u64 deadline = timeout_ns ? rk_time_ns() + timeout_ns : 0;
	for (;;) {
		u64 v = rk_notify_poll(n, mask);
		if (v)
			return v;
		if (deadline && rk_time_ns() > deadline)
			return 0;
		waitq_wait_timeout(&n->wq, RK_NS_PER_MS);
	}
}

void rk_ipc_stats(struct ipc_stats *out)
{
	unsigned long f = spin_lock_irqsave(&istats_lock);
	*out = istats;
	spin_unlock_irqrestore(&istats_lock, f);
}

void rk_ipc_init(void)
{
	spin_lock_init(&istats_lock, "ipc-stats");
	endpoint_cache = kmem_cache_create("endpoint", sizeof(struct rk_endpoint), 32);
	channel_cache  = kmem_cache_create("channel", sizeof(struct rk_channel), 32);
	notify_cache   = kmem_cache_create("notify", sizeof(struct rk_notify), 32);
	pr_info("IPC ready: endpoints, channels, notifications");
}
