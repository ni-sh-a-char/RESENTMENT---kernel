/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the runtime graph.
 *
 * Modelled on WebWeaveX (https://github.com/ni-sh-a-char/WebWeaveX), which
 * captures a running system as a deterministic graph with stable node
 * identities rather than as an opaque snapshot. WebWeaveX does this for web
 * applications so that an agent can reason about them. RESENTMENT does it for
 * the operating system itself.
 *
 * Every object the kernel creates - task, thread, capability, mapping, file,
 * device, model, channel - is a node. Every relation between them is an edge.
 * Each node carries a SHA-256 digest computed over a canonical encoding of its
 * own fields plus the sorted digests of its children, so the graph is a Merkle
 * DAG and the whole machine has one root digest.
 *
 * That single property buys four things a normal kernel cannot offer:
 *
 *   introspection  an agent reads system state as a typed graph, not by
 *                  scraping the text output of a dozen tools
 *   diffing        two machines, or one machine at two instants, are compared
 *                  by walking digests; identical subtrees are skipped
 *   attestation    the root digest is Kaalka-sealed, so a snapshot proves what
 *                  the system looked like and when
 *   replay         events are recorded in causal order and can be re-applied
 *                  to reconstruct an identical state, which is what makes a
 *                  crash reproducible instead of anecdotal
 *
 * Cost control matters: this runs inside a kernel. Digests are computed lazily
 * and cached, invalidation propagates only along parent links, and the event
 * log is a fixed-size ring. Recording can be disabled per subsystem at runtime.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/crypto.h>
#include <rk/kaalka.h>
#include <rk/spinlock.h>

#define RK_GRAPH_DIGEST_SIZE 32
#define RK_GRAPH_LABEL_MAX   48
#define RK_GRAPH_MAX_ATTRS   12

/* ---------------------------------------------------------------- nodes */

enum graph_node_kind {
	GNODE_ROOT = 0,
	GNODE_MACHINE,
	GNODE_CPU,
	GNODE_MEMORY,
	GNODE_TASK,
	GNODE_THREAD,
	GNODE_ADDRSPACE,
	GNODE_MAPPING,
	GNODE_CAPSPACE,
	GNODE_CAPABILITY,
	GNODE_ENDPOINT,
	GNODE_CHANNEL,
	GNODE_FILE,
	GNODE_DIRECTORY,
	GNODE_DEVICE,
	GNODE_DRIVER,
	GNODE_IRQ,
	GNODE_MODEL,
	GNODE_TENSOR,
	GNODE_INFERENCE,
	GNODE_ACCEL,
	GNODE_SCRIPT,        /* a SHE program */
	GNODE_AGENT,
	GNODE_LOGSPAN,
	GNODE_SNAPSHOT,
	GNODE_KIND_COUNT
};

/* An attribute is a small typed key/value. Kept fixed-size and inline so that
 * building the graph never allocates on a hot path. */
enum graph_attr_type { GATTR_NONE = 0, GATTR_U64, GATTR_S64, GATTR_STR, GATTR_BOOL, GATTR_DIGEST };

struct graph_attr {
	char  key[16];
	u8    type;
	union {
		u64  u;
		s64  s;
		bool b;
		char str[48];
		u8   digest[RK_GRAPH_DIGEST_SIZE];
	} v;
};

struct graph_node {
	rk_id_t              id;
	u16                  kind;
	u16                  nattrs;
	u32                  flags;

	char                 label[RK_GRAPH_LABEL_MAX];
	struct graph_attr    attrs[RK_GRAPH_MAX_ATTRS];

	struct graph_node   *parent;
	struct list_head     children;
	struct list_head     sibling;
	struct list_head     edges_out;
	struct list_head     edges_in;

	u8                   digest[RK_GRAPH_DIGEST_SIZE];
	bool                 digest_valid;

	u64                  created_ns;
	u64                  updated_ns;
	u64                  version;      /* bumped on every mutation */
	void                *owner;        /* back-pointer to the kernel object */
	spinlock_t           lock;
};

#define GNODE_F_PINNED    (1u << 0)   /* never garbage collected */
#define GNODE_F_SENSITIVE (1u << 1)   /* attributes redacted in exports */
#define GNODE_F_EPHEMERAL (1u << 2)   /* excluded from snapshots */

/* ---------------------------------------------------------------- edges */

enum graph_edge_kind {
	GEDGE_CONTAINS = 0,
	GEDGE_OWNS,
	GEDGE_MAPS,
	GEDGE_HOLDS_CAP,
	GEDGE_DERIVED_FROM,
	GEDGE_SENDS_TO,
	GEDGE_BLOCKED_ON,
	GEDGE_RUNS_ON,
	GEDGE_READS,
	GEDGE_WRITES,
	GEDGE_INFERS_WITH,
	GEDGE_CAUSED_BY,
	GEDGE_SNAPSHOT_OF,
	GEDGE_KIND_COUNT
};

struct graph_edge {
	struct list_head   from_link;
	struct list_head   to_link;
	struct graph_node *from;
	struct graph_node *to;
	u16                kind;
	u64                weight;
	u64                created_ns;
};

/* ------------------------------------------------------------------- API */

void rk_graph_init(void);

struct graph_node *rk_graph_root(void);
struct graph_node *rk_graph_node_create(enum graph_node_kind kind, const char *label,
                                        struct graph_node *parent, void *owner);
void rk_graph_node_destroy(struct graph_node *n);
struct graph_node *rk_graph_node_find(rk_id_t id);

void rk_graph_set_u64(struct graph_node *n, const char *key, u64 v);
void rk_graph_set_s64(struct graph_node *n, const char *key, s64 v);
void rk_graph_set_str(struct graph_node *n, const char *key, const char *v);
void rk_graph_set_bool(struct graph_node *n, const char *key, bool v);
bool rk_graph_get_u64(const struct graph_node *n, const char *key, u64 *out);

struct graph_edge *rk_graph_link(struct graph_node *from, struct graph_node *to,
                                 enum graph_edge_kind kind);
void rk_graph_unlink(struct graph_edge *e);

/* Recompute the Merkle digest for n and everything under it, then invalidate
 * ancestors so the root digest stays correct. */
const u8 *rk_graph_digest(struct graph_node *n);
const u8 *rk_graph_root_digest(void);
void      rk_graph_touch(struct graph_node *n);

/* ------------------------------------------------------- canonical export */

/* Deterministic serialisation. Attributes are emitted in sorted key order,
 * integers as fixed-width big-endian, strings length-prefixed, children in
 * digest order. Two machines in the same state produce identical bytes, which
 * is the whole point: equality of state is equality of hash. */
enum graph_format {
	GRAPH_FMT_CANON = 0,   /* compact binary, the hashing input */
	GRAPH_FMT_JSON,        /* agent-facing, still deterministic */
	GRAPH_FMT_TEXT,        /* human-facing tree */
	GRAPH_FMT_DOT,         /* graphviz */
};

/* Writes at most cap bytes, returns the number of bytes the full output needs
 * (so a caller can size a buffer with one probing call). */
size_t rk_graph_export(struct graph_node *n, enum graph_format fmt,
                       int max_depth, char *out, size_t cap);

/* --------------------------------------------------------------- events */

/* The causal log. Every entry is ordered, timestamped and attributed to a
 * thread, which is what allows a run to be replayed rather than guessed at. */
enum graph_event_kind {
	GEV_NODE_CREATE = 0,
	GEV_NODE_UPDATE,
	GEV_NODE_DESTROY,
	GEV_EDGE_ADD,
	GEV_EDGE_DEL,
	GEV_SYSCALL,
	GEV_SCHED_SWITCH,
	GEV_IRQ,
	GEV_FAULT,
	GEV_CAP_CHECK,
	GEV_IPC,
	GEV_INFERENCE,
	GEV_LOG,
	GEV_KIND_COUNT
};

struct graph_event {
	u64 seq;
	u64 mono_ns;
	u32 kind;
	u32 cpu;
	rk_id_t tid;
	rk_id_t node;
	u64 a, b, c;
};

#define RK_GRAPH_EVENT_RING 8192

void rk_graph_record(enum graph_event_kind kind, rk_id_t node, u64 a, u64 b, u64 c);
void rk_graph_record_enable(enum graph_event_kind kind, bool on);
size_t rk_graph_events_read(struct graph_event *out, size_t max, u64 *cursor);
u64  rk_graph_event_seq(void);

/* ------------------------------------------------------------- snapshots */

/* A snapshot is a canonical export plus a Kaalka seal over its digest. It
 * answers "what did this machine look like, and when, and can you prove it".
 */
struct graph_snapshot {
	u8                 root_digest[RK_GRAPH_DIGEST_SIZE];
	struct kaalka_seal seal;
	u64                mono_ns;
	s64                unix_sec;
	u64                event_seq;
	u32                node_count;
	u32                byte_len;
	rk_id_t            id;
};

int rk_graph_snapshot(struct graph_snapshot *out, void *buf, size_t cap);
int rk_graph_snapshot_verify(const struct graph_snapshot *s, const void *buf, size_t len);

/* Compare two exports and emit a line-oriented diff of what changed. */
size_t rk_graph_diff(const void *a, size_t alen, const void *b, size_t blen,
                     char *out, size_t cap);

/* ---------------------------------------------------------------- replay */

/* Replay drives the kernel from a recorded event stream instead of from
 * hardware: the clock is virtual, entropy is replayed, and any divergence
 * from the recorded digests is reported as RK_EDETERM rather than silently
 * producing a different run. */
int  rk_graph_replay_begin(const struct graph_event *events, size_t count);
int  rk_graph_replay_step(void);
void rk_graph_replay_end(void);
bool rk_graph_replaying(void);

/* --------------------------------------------------- federated memory fabric */

/* A deterministic key/value store keyed by digest. WebWeaveX merges execution
 * histories across turns; here the same idea merges state across boots, so an
 * agent keeps continuity over a reboot without a filesystem convention.
 * Entries are content-addressed, so a merge is idempotent by construction. */
void   rk_memfab_init(u64 cap_bytes);
int    rk_memfab_put(const u8 key[RK_GRAPH_DIGEST_SIZE], const void *val, size_t len);
size_t rk_memfab_get(const u8 key[RK_GRAPH_DIGEST_SIZE], void *val, size_t cap);
int    rk_memfab_put_str(const char *key, const void *val, size_t len);
size_t rk_memfab_get_str(const char *key, void *val, size_t cap);
int    rk_memfab_merge(const void *blob, size_t len);
size_t rk_memfab_serialize(void *out, size_t cap);
void   rk_memfab_stats(u64 *entries, u64 *bytes, u64 *hits, u64 *misses);

struct graph_stats {
	u64 nodes, edges, events;
	u64 digest_computes, digest_cache_hits;
	u64 snapshots, replays, determinism_faults;
	u64 bytes_exported;
};
void rk_graph_stats(struct graph_stats *out);

const char *graph_node_kind_name(enum graph_node_kind k);
const char *graph_edge_kind_name(enum graph_edge_kind k);
const char *graph_event_kind_name(enum graph_event_kind k);
