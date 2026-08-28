/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - capabilities.
 *
 * There is no ambient authority in RESENTMENT. A task cannot open a file
 * because it knows a path; it can only act on objects it holds a capability
 * for. This is the same model the SHE language exposes to programmers
 * (https://github.com/ni-sh-a-char/SHE), except that here the sandbox is
 * enforced by the kernel rather than by an interpreter, so a native binary,
 * a SHE script and an AI agent are all constrained by one mechanism.
 *
 * A capability is:
 *
 *   object   what it refers to
 *   rights   what may be done to it, monotonically decreasing on derivation
 *   badge    an opaque value the holder cannot forge, delivered to the server
 *   seal     a Kaalka temporal seal; expired capabilities fail closed
 *
 * Derivation only ever removes rights and only ever shortens lifetime, so the
 * authority reachable from a capability is bounded by the capability itself.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/kaalka.h>
#include <rk/spinlock.h>

/* ------------------------------------------------------------ object kinds */

enum cap_type {
	CAP_NULL = 0,
	CAP_TASK,          /* control another task */
	CAP_THREAD,
	CAP_MEMORY,        /* a vm_object */
	CAP_ADDRSPACE,
	CAP_ENDPOINT,      /* IPC rendezvous */
	CAP_CHANNEL,       /* buffered IPC */
	CAP_NOTIFY,        /* async signal */
	CAP_FILE,          /* a VFS node */
	CAP_DIR,
	CAP_DEVICE,
	CAP_IRQ,
	CAP_IOPORT,
	CAP_MMIO,
	CAP_TIMER,
	CAP_GRAPH,         /* a runtime-graph view */
	CAP_MODEL,         /* an AI model */
	CAP_TENSOR,
	CAP_ACCEL,         /* a compute accelerator queue */
	CAP_ENTROPY,
	CAP_CLOCK,         /* read wall time; yes, even the clock is a capability */
	CAP_NET,
	CAP_CONSOLE,
	CAP_CAPSPACE,      /* grant into another task */
	CAP_NTYPES
};

/* ---------------------------------------------------------------- rights */

#define CAP_RIGHT_READ      (1u << 0)
#define CAP_RIGHT_WRITE     (1u << 1)
#define CAP_RIGHT_EXEC      (1u << 2)
#define CAP_RIGHT_MAP       (1u << 3)
#define CAP_RIGHT_SEND      (1u << 4)
#define CAP_RIGHT_RECV      (1u << 5)
#define CAP_RIGHT_GRANT     (1u << 6)   /* may hand a copy to another task */
#define CAP_RIGHT_DERIVE    (1u << 7)   /* may mint a weaker child */
#define CAP_RIGHT_REVOKE    (1u << 8)
#define CAP_RIGHT_CREATE    (1u << 9)
#define CAP_RIGHT_DELETE    (1u << 10)
#define CAP_RIGHT_CONTROL   (1u << 11)  /* start, stop, kill */
#define CAP_RIGHT_INSPECT   (1u << 12)  /* observe without modifying */
#define CAP_RIGHT_INFER     (1u << 13)  /* run a model */
#define CAP_RIGHT_TRAIN     (1u << 14)  /* update model weights */
#define CAP_RIGHT_SNAPSHOT  (1u << 15)  /* capture graph state */
#define CAP_RIGHT_REPLAY    (1u << 16)  /* drive the system from a recording */
#define CAP_RIGHT_ALL       0x0001ffffu

/* --------------------------------------------------------------- objects */

struct cap_object {
	enum cap_type type;
	rk_id_t       id;
	void         *ptr;             /* the underlying kernel object */
	u32           refcount;
	u32           generation;      /* bumped on revoke: stale handles fail */
	const char   *label;           /* human and agent readable */
	void        (*release)(void *ptr);
	struct list_head derived;      /* children, for recursive revocation */
	struct list_head sibling;
	spinlock_t    lock;
};

/* --------------------------------------------------------- the capability */

struct capability {
	struct cap_object *obj;
	u32                rights;
	u32                generation;
	u64                badge;
	struct kaalka_seal seal;
	u64                uses_left;    /* 0 = unlimited; else single/N-shot */
	bool               valid;
};

/* ---------------------------------------------------------- capability space */

#define CAPSPACE_INIT_SLOTS 64
#define CAPSPACE_MAX_SLOTS  65536

typedef s32 cap_handle_t;
#define CAP_INVALID ((cap_handle_t)-1)

struct capspace {
	struct capability *slots;
	u32                nslots;
	u32                used;
	spinlock_t         lock;
	u32                refcount;
	rk_id_t            id;
	struct kaalka_ledger ledger;   /* replay defence for sealed grants */
};

void  cap_init(void);

struct capspace *capspace_create(void);
void  capspace_destroy(struct capspace *cs);
void  capspace_get(struct capspace *cs);

struct cap_object *cap_object_create(enum cap_type type, void *ptr,
                                     const char *label, void (*release)(void *));
void  cap_object_get(struct cap_object *o);
void  cap_object_put(struct cap_object *o);

/* Install a capability, returning its handle in that capspace. */
cap_handle_t cap_install(struct capspace *cs, struct cap_object *obj,
                         u32 rights, u64 badge, u64 lifetime_sec);

/* Look up and validate in one step: type, rights, generation, and Kaalka seal
 * are all checked. This is the only way kernel code should reach an object. */
int cap_lookup(struct capspace *cs, cap_handle_t h, enum cap_type type,
               u32 need_rights, struct cap_object **out) __must_check;

/* Derive a weaker capability. new_rights is intersected with the parent, and
 * lifetime is clamped to the parent remaining lifetime. */
cap_handle_t cap_derive(struct capspace *cs, cap_handle_t parent,
                        u32 new_rights, u64 new_badge, u64 lifetime_sec);

/* Copy a capability into another capspace, optionally weakening it. Requires
 * CAP_RIGHT_GRANT on the source. */
cap_handle_t cap_grant(struct capspace *from, cap_handle_t h,
                       struct capspace *to, u32 rights_mask);

int  cap_revoke(struct capspace *cs, cap_handle_t h);
int  cap_revoke_recursive(struct cap_object *obj);
int  cap_close(struct capspace *cs, cap_handle_t h);
int  cap_rights(struct capspace *cs, cap_handle_t h, u32 *out);
int  cap_badge(struct capspace *cs, cap_handle_t h, u64 *out);

const char *cap_type_name(enum cap_type t);
size_t      cap_rights_string(char *buf, size_t n, u32 rights);
u32         cap_rights_parse(const char *s);

struct cap_stats {
	u64 objects_live, caps_live;
	u64 installs, derives, grants, revokes;
	u64 denials, expiries, generation_faults;
};
void cap_stats(struct cap_stats *out);

/* Enumerate for introspection and for the runtime graph. */
typedef bool (*cap_iter_fn)(cap_handle_t h, const struct capability *c, void *ctx);
void cap_foreach(struct capspace *cs, cap_iter_fn fn, void *ctx);
