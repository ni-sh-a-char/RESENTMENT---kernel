/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - inter-process communication.
 *
 * Three mechanisms, each the right shape for a different job:
 *
 *   endpoint  synchronous call/reply rendezvous. The caller blocks, the reply
 *             capability is single-use, and the scheduler hands the CPU
 *             directly to the server without a trip through the run queue.
 *             This is the fast path for system services.
 *   channel   buffered asynchronous stream with credit-based flow control, so
 *             a fast producer cannot exhaust kernel memory against a slow
 *             consumer. Used for logs, events and token streams.
 *   notify    a word of signal bits, never blocks, never allocates. Used by
 *             drivers and by the timer.
 *
 * Messages carry inline bytes plus capabilities. Large payloads move as a
 * memory capability rather than a copy, so zero-copy is the default rather
 * than an optimisation.
 *
 * Every message may optionally be Kaalka-sealed, which gives cross-machine
 * IPC replay protection and temporal validity with no extra protocol.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/sync.h>
#include <rk/cap.h>
#include <rk/kaalka.h>

#define RK_MSG_INLINE_MAX 256
#define RK_MSG_CAPS_MAX   4

struct rk_message {
	u64          label;          /* opaque to the kernel, the RPC selector */
	u32          len;            /* inline bytes used */
	u32          ncaps;
	u64          badge;          /* filled in by the kernel from the cap */
	u8           data[RK_MSG_INLINE_MAX];
	cap_handle_t caps[RK_MSG_CAPS_MAX];
	u64          seq;
	bool         sealed;
	struct kaalka_seal seal;
};

/* ------------------------------------------------------------- endpoints */

struct rk_endpoint {
	rk_id_t          id;
	char             name[32];
	struct list_head senders;      /* threads blocked in call/send */
	struct list_head receivers;    /* threads blocked in recv */
	spinlock_t       lock;
	u32              refcount;
	u64              calls, replies, timeouts;
	rk_id_t          graph_node;
	struct kaalka_ledger ledger;
	bool             require_seal;
};

struct rk_endpoint *rk_endpoint_create(const char *name);
void rk_endpoint_get(struct rk_endpoint *e);
void rk_endpoint_put(struct rk_endpoint *e);

/* Synchronous call: send and block until the server replies. */
int rk_ipc_call(struct rk_endpoint *e, struct rk_message *msg, u64 timeout_ns);
/* Server side: block for a request, returns a single-use reply handle. */
int rk_ipc_recv(struct rk_endpoint *e, struct rk_message *msg,
                cap_handle_t *reply_out, u64 timeout_ns);
int rk_ipc_reply(cap_handle_t reply, struct rk_message *msg);
/* Reply to the current request and immediately wait for the next one. This is
 * the loop a server actually runs, and doing it in one syscall halves the
 * switches per request. */
int rk_ipc_reply_recv(struct rk_endpoint *e, cap_handle_t reply,
                      struct rk_message *msg, cap_handle_t *next_reply);
int rk_ipc_send(struct rk_endpoint *e, struct rk_message *msg, u64 timeout_ns);

/* --------------------------------------------------------------- channels */

struct rk_channel {
	rk_id_t          id;
	char             name[32];
	u8              *ring;
	size_t           capacity;
	size_t           head, tail;
	u32              credits;        /* flow control */
	u32              max_credits;
	struct waitqueue readers, writers;
	spinlock_t       lock;
	u32              refcount;
	bool             closed;
	u64              bytes_in, bytes_out, drops;
	rk_id_t          graph_node;
};

struct rk_channel *rk_channel_create(const char *name, size_t capacity);
void rk_channel_get(struct rk_channel *c);
void rk_channel_put(struct rk_channel *c);
ssize_t rk_channel_write(struct rk_channel *c, const void *buf, size_t n, u64 timeout_ns);
ssize_t rk_channel_read(struct rk_channel *c, void *buf, size_t n, u64 timeout_ns);
void rk_channel_close(struct rk_channel *c);
size_t rk_channel_available(struct rk_channel *c);

/* ------------------------------------------------------------- notifications */

struct rk_notify {
	rk_id_t          id;
	char             name[32];
	atomic64_t       bits;
	struct waitqueue wq;
	u32              refcount;
	rk_id_t          graph_node;
};

struct rk_notify *rk_notify_create(const char *name);
void rk_notify_get(struct rk_notify *n);
void rk_notify_put(struct rk_notify *n);
void rk_notify_signal(struct rk_notify *n, u64 bits);  /* IRQ-safe */
u64  rk_notify_wait(struct rk_notify *n, u64 mask, u64 timeout_ns);
u64  rk_notify_poll(struct rk_notify *n, u64 mask);

/* ------------------------------------------------------------- shared memory */

int rk_shm_create(const char *name, size_t bytes, cap_handle_t *out);
int rk_shm_open(const char *name, u32 rights, cap_handle_t *out);

struct ipc_stats {
	u64 calls, sends, recvs, replies;
	u64 channel_bytes, notifications;
	u64 blocked_now, timeouts, seal_failures;
	u64 fastpath_switches;
};
void rk_ipc_stats(struct ipc_stats *out);

void rk_ipc_init(void);
