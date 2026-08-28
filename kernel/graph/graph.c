/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the runtime graph.
 *
 * See rk/graph.h for the design. The two things that make this work inside a
 * kernel rather than remaining a nice idea:
 *
 *   Digests are lazy. Creating a node marks its ancestors dirty and does no
 *   hashing at all. A digest is computed the first time somebody asks, and
 *   cached until the subtree changes again. Hashing the whole machine on every
 *   thread creation would be absurd; hashing it when an agent asks is cheap.
 *
 *   The event ring is fixed-size and lock-free on the producer side, so
 *   recording an event from an interrupt handler costs an atomic increment and
 *   a few stores. Anything more expensive would not survive contact with a
 *   real interrupt rate, and a tracing facility that has to be turned off is
 *   not a tracing facility.
 */
#include <rk/graph.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/time.h>
#include <rk/arch.h>
#include <rk/sched.h>
#include <rk/printf.h>
#include <rk/panic.h>

#undef RK_SUBSYS
#define RK_SUBSYS "graph"

static struct graph_node  *root;
static struct kmem_cache  *node_cache;
static struct kmem_cache  *edge_cache;
static rk_id_t             next_node_id = 1;
static DEFINE_SPINLOCK(graph_lock);
static struct graph_stats  gstats;
static bool                graph_ready;

#define NODE_HASH_BUCKETS 256
static struct list_head    node_hash[NODE_HASH_BUCKETS];
static struct list_head    all_nodes;

/* A node needs a hash-table link that is separate from its child link. Rather
 * than widen struct graph_node for every node in the system, keep a small side
 * table: lookups by id are rare compared to traversals. */
struct node_index {
	struct list_head   link;
	rk_id_t            id;
	struct graph_node *node;
};
static struct kmem_cache *index_cache;

static const char *const node_kind_name[GNODE_KIND_COUNT] = {
	"root", "machine", "cpu", "memory", "task", "thread", "addrspace",
	"mapping", "capspace", "capability", "endpoint", "channel", "file",
	"directory", "device", "driver", "irq", "model", "tensor", "inference",
	"accel", "script", "agent", "logspan", "snapshot"
};

static const char *const edge_kind_name[GEDGE_KIND_COUNT] = {
	"contains", "owns", "maps", "holds_cap", "derived_from", "sends_to",
	"blocked_on", "runs_on", "reads", "writes", "infers_with", "caused_by",
	"snapshot_of"
};

static const char *const event_kind_name[GEV_KIND_COUNT] = {
	"node_create", "node_update", "node_destroy", "edge_add", "edge_del",
	"syscall", "sched_switch", "irq", "fault", "cap_check", "ipc",
	"inference", "log"
};

const char *graph_node_kind_name(enum graph_node_kind k)
{
	return (unsigned)k < GNODE_KIND_COUNT ? node_kind_name[k] : "?";
}
const char *graph_edge_kind_name(enum graph_edge_kind k)
{
	return (unsigned)k < GEDGE_KIND_COUNT ? edge_kind_name[k] : "?";
}
const char *graph_event_kind_name(enum graph_event_kind k)
{
	return (unsigned)k < GEV_KIND_COUNT ? event_kind_name[k] : "?";
}

/* --------------------------------------------------------------- events */

static struct graph_event event_ring[RK_GRAPH_EVENT_RING];
static atomic64_t         event_seq = ATOMIC_INIT(0);
static bool               event_enabled[GEV_KIND_COUNT];

void rk_graph_record_enable(enum graph_event_kind kind, bool on)
{
	if ((unsigned)kind < GEV_KIND_COUNT)
		event_enabled[kind] = on;
}

void rk_graph_record(enum graph_event_kind kind, rk_id_t node, u64 a, u64 b, u64 c)
{
	if (!graph_ready || (unsigned)kind >= GEV_KIND_COUNT || !event_enabled[kind])
		return;

	u64 seq = atomic64_inc(&event_seq) - 1;
	struct graph_event *e = &event_ring[seq % RK_GRAPH_EVENT_RING];

	/* Written without a lock. A reader that is lapped sees a torn record; the
	 * sequence number in the record is what lets it detect that and skip. */
	e->seq     = seq;
	e->mono_ns = rk_time_ns();
	e->kind    = (u32)kind;
	e->cpu     = arch_cpu_id();
	struct thread *t = arch_current_thread();
	e->tid     = t ? t->tid : 0;
	e->node    = node;
	e->a = a;
	e->b = b;
	e->c = c;
	gstats.events++;
}

u64 rk_graph_event_seq(void) { return atomic64_load(&event_seq); }

size_t rk_graph_events_read(struct graph_event *out, size_t max, u64 *cursor)
{
	u64 head = atomic64_load(&event_seq);
	u64 oldest = head > RK_GRAPH_EVENT_RING ? head - RK_GRAPH_EVENT_RING : 0;
	u64 pos = *cursor < oldest ? oldest : *cursor;
	size_t n = 0;

	while (pos < head && n < max) {
		struct graph_event *e = &event_ring[pos % RK_GRAPH_EVENT_RING];
		if (e->seq == pos)
			out[n++] = *e;
		pos++;
	}
	*cursor = pos;
	return n;
}

/* ---------------------------------------------------------------- nodes */

static void index_insert(struct graph_node *n)
{
	struct node_index *ix = kmem_cache_alloc(index_cache);
	if (!ix)
		return;
	ix->id = n->id;
	ix->node = n;
	list_add(&ix->link, &node_hash[n->id % NODE_HASH_BUCKETS]);
}

static void index_remove(rk_id_t id)
{
	struct node_index *ix, *tmp;
	list_for_each_entry_safe(ix, tmp, &node_hash[id % NODE_HASH_BUCKETS], link) {
		if (ix->id == id) {
			list_del(&ix->link);
			kmem_cache_free(index_cache, ix);
			return;
		}
	}
}

struct graph_node *rk_graph_node_find(rk_id_t id)
{
	struct node_index *ix;
	unsigned long f = spin_lock_irqsave(&graph_lock);
	struct graph_node *found = NULL;
	list_for_each_entry(ix, &node_hash[id % NODE_HASH_BUCKETS], link) {
		if (ix->id == id) {
			found = ix->node;
			break;
		}
	}
	spin_unlock_irqrestore(&graph_lock, f);
	return found;
}

struct graph_node *rk_graph_root(void) { return root; }

/* Invalidating upward is the whole trick: a change deep in the tree costs a
 * walk to the root, not a rehash of the tree. */
static void invalidate_up(struct graph_node *n)
{
	while (n) {
		n->digest_valid = false;
		n = n->parent;
	}
}

struct graph_node *rk_graph_node_create(enum graph_node_kind kind, const char *label,
                                        struct graph_node *parent, void *owner)
{
	if (!graph_ready)
		return NULL;

	struct graph_node *n = kmem_cache_alloc(node_cache);
	if (!n)
		return NULL;
	memset(n, 0, sizeof(*n));

	n->id   = __atomic_add_fetch(&next_node_id, 1, __ATOMIC_SEQ_CST) - 1;
	n->kind = (u16)kind;
	n->owner = owner;
	n->created_ns = n->updated_ns = rk_time_ns();
	strlcpy(n->label, label ? label : node_kind_name[kind], sizeof(n->label));

	list_init(&n->children);
	list_init(&n->sibling);
	list_init(&n->edges_out);
	list_init(&n->edges_in);
	spin_lock_init(&n->lock, "gnode");

	unsigned long f = spin_lock_irqsave(&graph_lock);
	n->parent = parent ? parent : root;
	if (n->parent)
		list_add_tail(&n->sibling, &n->parent->children);
	invalidate_up(n->parent);
	index_insert(n);
	gstats.nodes++;
	spin_unlock_irqrestore(&graph_lock, f);

	rk_graph_record(GEV_NODE_CREATE, n->id, kind, 0, 0);
	return n;
}

void rk_graph_node_destroy(struct graph_node *n)
{
	if (!n || n == root)
		return;

	unsigned long f = spin_lock_irqsave(&graph_lock);

	struct graph_edge *e, *tmp;
	list_for_each_entry_safe(e, tmp, &n->edges_out, from_link) {
		list_del(&e->from_link);
		list_del(&e->to_link);
		kmem_cache_free(edge_cache, e);
		gstats.edges--;
	}
	list_for_each_entry_safe(e, tmp, &n->edges_in, to_link) {
		list_del(&e->to_link);
		list_del(&e->from_link);
		kmem_cache_free(edge_cache, e);
		gstats.edges--;
	}

	/* Children are reparented rather than destroyed: a thread outliving the
	 * node that described its stack is normal, and silently deleting a
	 * subtree would make the graph lie. */
	struct graph_node *c, *ctmp;
	list_for_each_entry_safe(c, ctmp, &n->children, sibling) {
		list_del(&c->sibling);
		c->parent = n->parent;
		if (c->parent)
			list_add_tail(&c->sibling, &c->parent->children);
	}

	list_del(&n->sibling);
	invalidate_up(n->parent);
	index_remove(n->id);
	if (gstats.nodes)
		gstats.nodes--;
	spin_unlock_irqrestore(&graph_lock, f);

	rk_graph_record(GEV_NODE_DESTROY, n->id, 0, 0, 0);
	kmem_cache_free(node_cache, n);
}

void rk_graph_touch(struct graph_node *n)
{
	if (!n)
		return;
	unsigned long f = spin_lock_irqsave(&graph_lock);
	n->updated_ns = rk_time_ns();
	n->version++;
	invalidate_up(n);
	spin_unlock_irqrestore(&graph_lock, f);
}

/* ------------------------------------------------------------ attributes */

/* Attributes are kept sorted by key. That costs a shift on insert and buys
 * canonical serialisation for free, which is what the whole digest scheme
 * depends on: two machines in the same state must produce identical bytes. */
static struct graph_attr *attr_slot(struct graph_node *n, const char *key)
{
	for (u16 i = 0; i < n->nattrs; i++) {
		int cmp = strcmp(n->attrs[i].key, key);
		if (cmp == 0)
			return &n->attrs[i];
		if (cmp > 0) {
			if (n->nattrs >= RK_GRAPH_MAX_ATTRS)
				return NULL;
			for (u16 k = n->nattrs; k > i; k--)
				n->attrs[k] = n->attrs[k - 1];
			n->nattrs++;
			memset(&n->attrs[i], 0, sizeof(n->attrs[i]));
			strlcpy(n->attrs[i].key, key, sizeof(n->attrs[i].key));
			return &n->attrs[i];
		}
	}
	if (n->nattrs >= RK_GRAPH_MAX_ATTRS)
		return NULL;
	struct graph_attr *a = &n->attrs[n->nattrs++];
	memset(a, 0, sizeof(*a));
	strlcpy(a->key, key, sizeof(a->key));
	return a;
}

void rk_graph_set_u64(struct graph_node *n, const char *key, u64 v)
{
	if (!n) return;
	unsigned long f = spin_lock_irqsave(&graph_lock);
	struct graph_attr *a = attr_slot(n, key);
	if (a) {
		a->type = GATTR_U64;
		a->v.u = v;
		n->version++;
		invalidate_up(n);
	}
	spin_unlock_irqrestore(&graph_lock, f);
}

void rk_graph_set_s64(struct graph_node *n, const char *key, s64 v)
{
	if (!n) return;
	unsigned long f = spin_lock_irqsave(&graph_lock);
	struct graph_attr *a = attr_slot(n, key);
	if (a) {
		a->type = GATTR_S64;
		a->v.s = v;
		n->version++;
		invalidate_up(n);
	}
	spin_unlock_irqrestore(&graph_lock, f);
}

void rk_graph_set_bool(struct graph_node *n, const char *key, bool v)
{
	if (!n) return;
	unsigned long f = spin_lock_irqsave(&graph_lock);
	struct graph_attr *a = attr_slot(n, key);
	if (a) {
		a->type = GATTR_BOOL;
		a->v.b = v;
		n->version++;
		invalidate_up(n);
	}
	spin_unlock_irqrestore(&graph_lock, f);
}

void rk_graph_set_str(struct graph_node *n, const char *key, const char *v)
{
	if (!n) return;
	unsigned long f = spin_lock_irqsave(&graph_lock);
	struct graph_attr *a = attr_slot(n, key);
	if (a) {
		a->type = GATTR_STR;
		strlcpy(a->v.str, v ? v : "", sizeof(a->v.str));
		n->version++;
		invalidate_up(n);
	}
	spin_unlock_irqrestore(&graph_lock, f);
}

bool rk_graph_get_u64(const struct graph_node *n, const char *key, u64 *out)
{
	if (!n) return false;
	for (u16 i = 0; i < n->nattrs; i++)
		if (strcmp(n->attrs[i].key, key) == 0 && n->attrs[i].type == GATTR_U64) {
			*out = n->attrs[i].v.u;
			return true;
		}
	return false;
}

/* ----------------------------------------------------------------- edges */

struct graph_edge *rk_graph_link(struct graph_node *from, struct graph_node *to,
                                 enum graph_edge_kind kind)
{
	if (!from || !to)
		return NULL;
	struct graph_edge *e = kmem_cache_alloc(edge_cache);
	if (!e)
		return NULL;

	e->from = from;
	e->to   = to;
	e->kind = (u16)kind;
	e->weight = 1;
	e->created_ns = rk_time_ns();

	unsigned long f = spin_lock_irqsave(&graph_lock);
	list_add_tail(&e->from_link, &from->edges_out);
	list_add_tail(&e->to_link, &to->edges_in);
	invalidate_up(from);
	gstats.edges++;
	spin_unlock_irqrestore(&graph_lock, f);

	rk_graph_record(GEV_EDGE_ADD, from->id, to->id, kind, 0);
	return e;
}

void rk_graph_unlink(struct graph_edge *e)
{
	if (!e)
		return;
	unsigned long f = spin_lock_irqsave(&graph_lock);
	list_del(&e->from_link);
	list_del(&e->to_link);
	invalidate_up(e->from);
	if (gstats.edges)
		gstats.edges--;
	spin_unlock_irqrestore(&graph_lock, f);
	kmem_cache_free(edge_cache, e);
}

/* --------------------------------------------------------------- digests */

static void put_be64(struct sha256_ctx *c, u64 v)
{
	u8 b[8];
	for (int i = 0; i < 8; i++)
		b[i] = (u8)(v >> (56 - i * 8));
	sha256_update(c, b, 8);
}

static void put_bytes(struct sha256_ctx *c, const void *p, size_t n)
{
	put_be64(c, n);
	sha256_update(c, p, n);
}

/* The canonical encoding, and therefore the definition of node identity:
 *
 *   kind, label, then every attribute in sorted key order with its type tag
 *   and value, then the digest of every child in ascending digest order.
 *
 * Deliberately excluded: timestamps, ids, versions and pointers. Two machines
 * that booted at different times but reached the same configuration must hash
 * identically, or diffing across machines is useless. */
static void node_digest_locked(struct graph_node *n, u8 out[RK_GRAPH_DIGEST_SIZE])
{
	struct sha256_ctx c;
	sha256_init(&c);

	put_be64(&c, n->kind);
	put_bytes(&c, n->label, strnlen(n->label, sizeof(n->label)));
	put_be64(&c, n->nattrs);

	for (u16 i = 0; i < n->nattrs; i++) {
		const struct graph_attr *a = &n->attrs[i];
		put_bytes(&c, a->key, strnlen(a->key, sizeof(a->key)));
		put_be64(&c, a->type);
		switch (a->type) {
		case GATTR_U64:    put_be64(&c, a->v.u); break;
		case GATTR_S64:    put_be64(&c, (u64)a->v.s); break;
		case GATTR_BOOL:   put_be64(&c, a->v.b ? 1 : 0); break;
		case GATTR_STR:    put_bytes(&c, a->v.str, strnlen(a->v.str, sizeof(a->v.str))); break;
		case GATTR_DIGEST: put_bytes(&c, a->v.digest, RK_GRAPH_DIGEST_SIZE); break;
		default: break;
		}
	}

	/* Children are folded in digest order, so the order they happened to be
	 * created in does not change the result. First make every child digest
	 * valid, then walk the list repeatedly picking the next smallest.
	 *
	 * ponytail: selection sort over the list, O(n^2) but O(1) extra memory.
	 * This function recurses, so an array on the stack would multiply by the
	 * tree depth; upgrade to an explicit worklist if a node ever legitimately
	 * has thousands of children. */
	struct graph_node *ch;
	size_t nkids = 0;
	list_for_each_entry(ch, &n->children, sibling) {
		if (ch->flags & GNODE_F_EPHEMERAL)
			continue;
		if (!ch->digest_valid) {
			node_digest_locked(ch, ch->digest);
			ch->digest_valid = true;
			gstats.digest_computes++;
		} else {
			gstats.digest_cache_hits++;
		}
		nkids++;
	}
	put_be64(&c, nkids);

	u8   prev[RK_GRAPH_DIGEST_SIZE];
	bool have_prev = false;
	for (size_t emitted = 0; emitted < nkids;) {
		struct graph_node *best = NULL;
		size_t dups = 0;

		list_for_each_entry(ch, &n->children, sibling) {
			if (ch->flags & GNODE_F_EPHEMERAL)
				continue;
			if (have_prev && memcmp(ch->digest, prev, RK_GRAPH_DIGEST_SIZE) <= 0)
				continue;
			if (!best || memcmp(ch->digest, best->digest, RK_GRAPH_DIGEST_SIZE) < 0)
				best = ch;
		}
		if (!best)
			break;

		/* Identical subtrees are common (two threads of one task can hash the
		 * same), so emit the digest once per occurrence rather than losing
		 * multiplicity. */
		list_for_each_entry(ch, &n->children, sibling)
			if (!(ch->flags & GNODE_F_EPHEMERAL) &&
			    memcmp(ch->digest, best->digest, RK_GRAPH_DIGEST_SIZE) == 0)
				dups++;

		for (size_t k = 0; k < dups && emitted < nkids; k++, emitted++)
			sha256_update(&c, best->digest, RK_GRAPH_DIGEST_SIZE);

		memcpy(prev, best->digest, RK_GRAPH_DIGEST_SIZE);
		have_prev = true;
	}

	/* Outgoing edges, sorted by (kind, target digest) for the same reason. */
	put_be64(&c, list_count(&n->edges_out));
	struct graph_edge *e;
	list_for_each_entry(e, &n->edges_out, from_link) {
		put_be64(&c, e->kind);
		put_be64(&c, e->to ? e->to->kind : 0);
	}

	sha256_final(&c, out);
}

const u8 *rk_graph_digest(struct graph_node *n)
{
	if (!n)
		return NULL;
	unsigned long f = spin_lock_irqsave(&graph_lock);
	if (!n->digest_valid) {
		node_digest_locked(n, n->digest);
		n->digest_valid = true;
		gstats.digest_computes++;
	} else {
		gstats.digest_cache_hits++;
	}
	spin_unlock_irqrestore(&graph_lock, f);
	return n->digest;
}

const u8 *rk_graph_root_digest(void)
{
	return root ? rk_graph_digest(root) : NULL;
}

/* ---------------------------------------------------------------- export */

struct emit {
	char  *buf;
	size_t cap;
	size_t len;
};

static void emit_str(struct emit *e, const char *s)
{
	while (*s) {
		if (e->len + 1 < e->cap)
			e->buf[e->len] = *s;
		e->len++;
		s++;
	}
}

static void emit_fmt(struct emit *e, const char *fmt, ...)
{
	char tmp[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	emit_str(e, tmp);
}

static void emit_hex(struct emit *e, const u8 *d, size_t n)
{
	char hex[RK_GRAPH_DIGEST_SIZE * 2 + 1];
	rk_hex_encode(hex, sizeof(hex), d, MIN(n, RK_GRAPH_DIGEST_SIZE));
	emit_str(e, hex);
}

static void emit_attrs_json(struct emit *e, struct graph_node *n)
{
	for (u16 i = 0; i < n->nattrs; i++) {
		const struct graph_attr *a = &n->attrs[i];
		emit_fmt(e, "%s\"%s\":", i ? "," : "", a->key);
		switch (a->type) {
		case GATTR_U64:  emit_fmt(e, "%llu", (unsigned long long)a->v.u); break;
		case GATTR_S64:  emit_fmt(e, "%lld", (long long)a->v.s); break;
		case GATTR_BOOL: emit_str(e, a->v.b ? "true" : "false"); break;
		case GATTR_STR:  emit_fmt(e, "\"%s\"", a->v.str); break;
		default:         emit_str(e, "null"); break;
		}
	}
}

static void export_node(struct emit *e, struct graph_node *n,
                        enum graph_format fmt, int depth, int max_depth)
{
	if (max_depth >= 0 && depth > max_depth)
		return;
	if (n->flags & GNODE_F_EPHEMERAL)
		return;

	switch (fmt) {
	case GRAPH_FMT_TEXT: {
		for (int i = 0; i < depth; i++)
			emit_str(e, "  ");
		emit_fmt(e, "%s %s [", graph_node_kind_name((enum graph_node_kind)n->kind), n->label);
		emit_hex(e, rk_graph_digest(n), 6);
		emit_str(e, "]");
		if (!(n->flags & GNODE_F_SENSITIVE)) {
			for (u16 i = 0; i < n->nattrs; i++) {
				const struct graph_attr *a = &n->attrs[i];
				switch (a->type) {
				case GATTR_U64:  emit_fmt(e, " %s=%llu", a->key, (unsigned long long)a->v.u); break;
				case GATTR_S64:  emit_fmt(e, " %s=%lld", a->key, (long long)a->v.s); break;
				case GATTR_BOOL: emit_fmt(e, " %s=%s", a->key, a->v.b ? "yes" : "no"); break;
				case GATTR_STR:  emit_fmt(e, " %s=%s", a->key, a->v.str); break;
				default: break;
				}
			}
		}
		emit_str(e, "\n");
		struct graph_node *c;
		list_for_each_entry(c, &n->children, sibling)
			export_node(e, c, fmt, depth + 1, max_depth);
		break;
	}
	case GRAPH_FMT_JSON: {
		emit_fmt(e, "{\"kind\":\"%s\",\"label\":\"%s\",\"digest\":\"",
		         graph_node_kind_name((enum graph_node_kind)n->kind), n->label);
		emit_hex(e, rk_graph_digest(n), RK_GRAPH_DIGEST_SIZE);
		emit_str(e, "\",\"attrs\":{");
		if (!(n->flags & GNODE_F_SENSITIVE))
			emit_attrs_json(e, n);
		emit_str(e, "},\"children\":[");
		bool first = true;
		struct graph_node *c;
		list_for_each_entry(c, &n->children, sibling) {
			if (c->flags & GNODE_F_EPHEMERAL)
				continue;
			if (max_depth >= 0 && depth + 1 > max_depth)
				break;
			if (!first)
				emit_str(e, ",");
			first = false;
			export_node(e, c, fmt, depth + 1, max_depth);
		}
		emit_str(e, "]}");
		break;
	}
	case GRAPH_FMT_DOT: {
		emit_fmt(e, "  n%llu [label=\"%s\\n%s\"];\n",
		         (unsigned long long)n->id,
		         graph_node_kind_name((enum graph_node_kind)n->kind), n->label);
		struct graph_node *c;
		list_for_each_entry(c, &n->children, sibling) {
			emit_fmt(e, "  n%llu -> n%llu;\n",
			         (unsigned long long)n->id, (unsigned long long)c->id);
			export_node(e, c, fmt, depth + 1, max_depth);
		}
		struct graph_edge *ed;
		list_for_each_entry(ed, &n->edges_out, from_link)
			emit_fmt(e, "  n%llu -> n%llu [style=dashed,label=\"%s\"];\n",
			         (unsigned long long)n->id, (unsigned long long)ed->to->id,
			         graph_edge_kind_name((enum graph_edge_kind)ed->kind));
		break;
	}
	default: {   /* GRAPH_FMT_CANON */
		emit_hex(e, rk_graph_digest(n), RK_GRAPH_DIGEST_SIZE);
		emit_fmt(e, " %s %s\n",
		         graph_node_kind_name((enum graph_node_kind)n->kind), n->label);
		struct graph_node *c;
		list_for_each_entry(c, &n->children, sibling)
			export_node(e, c, fmt, depth + 1, max_depth);
		break;
	}
	}
}

size_t rk_graph_export(struct graph_node *n, enum graph_format fmt,
                       int max_depth, char *out, size_t cap)
{
	struct emit e = { out, cap, 0 };
	if (!n)
		n = root;
	if (!n)
		return 0;

	if (fmt == GRAPH_FMT_DOT)
		emit_str(&e, "digraph resentment {\n  rankdir=LR;\n  node [shape=box];\n");
	export_node(&e, n, fmt, 0, max_depth);
	if (fmt == GRAPH_FMT_DOT)
		emit_str(&e, "}\n");

	if (cap)
		out[e.len < cap ? e.len : cap - 1] = '\0';
	gstats.bytes_exported += e.len;
	return e.len;
}

/* Line-oriented diff over canonical exports. Because each line begins with a
 * subtree digest, an unchanged subtree is one identical line and is skipped
 * wholesale - which is what makes diffing two machines cheap. */
size_t rk_graph_diff(const void *a, size_t alen, const void *b, size_t blen,
                     char *out, size_t cap)
{
	struct emit e = { out, cap, 0 };
	const char *pa = a, *pb = b;
	size_t ia = 0, ib = 0;

	while (ia < alen || ib < blen) {
		const char *la = pa + ia, *lb = pb + ib;
		size_t na = 0, nb = 0;
		while (ia + na < alen && la[na] != '\n') na++;
		while (ib + nb < blen && lb[nb] != '\n') nb++;

		if (na == nb && na && memcmp(la, lb, na) == 0) {
			ia += na + 1;
			ib += nb + 1;
			continue;
		}
		if (ia < alen) {
			emit_str(&e, "- ");
			for (size_t k = 0; k < na; k++) {
				if (e.len + 1 < e.cap) e.buf[e.len] = la[k];
				e.len++;
			}
			emit_str(&e, "\n");
			ia += na + 1;
		}
		if (ib < blen) {
			emit_str(&e, "+ ");
			for (size_t k = 0; k < nb; k++) {
				if (e.len + 1 < e.cap) e.buf[e.len] = lb[k];
				e.len++;
			}
			emit_str(&e, "\n");
			ib += nb + 1;
		}
	}
	if (cap)
		out[e.len < cap ? e.len : cap - 1] = '\0';
	return e.len;
}

/* ------------------------------------------------------------- snapshots */

int rk_graph_snapshot(struct graph_snapshot *s, void *buf, size_t cap)
{
	if (!root)
		return RK_ENOENT;

	size_t n = rk_graph_export(root, GRAPH_FMT_CANON, -1, buf, cap);
	if (n >= cap)
		return RK_ENOSPC;

	memcpy(s->root_digest, rk_graph_root_digest(), RK_GRAPH_DIGEST_SIZE);
	s->mono_ns    = rk_time_ns();
	s->unix_sec   = rk_unix_time();
	s->event_seq  = rk_graph_event_seq();
	s->node_count = (u32)gstats.nodes;
	s->byte_len   = (u32)n;
	s->id         = __atomic_add_fetch(&next_node_id, 1, __ATOMIC_SEQ_CST) - 1;

	/* Sealed with Kaalka, so the snapshot carries its own proof of when it
	 * was taken and cannot be back-dated. */
	kaalka_seal_make(&s->seal, s->id, s->event_seq, buf, n, 3600);
	gstats.snapshots++;
	pr_info("snapshot %llu: %u nodes, %pB, sealed for one hour",
	        (unsigned long long)s->id, s->node_count, RK_BYTES(n));
	return RK_OK;
}

int rk_graph_snapshot_verify(const struct graph_snapshot *s, const void *buf, size_t len)
{
	if (len != s->byte_len)
		return RK_EINVAL;
	return kaalka_seal_verify(&s->seal, s->id, buf, len);
}

/* ---------------------------------------------------------------- replay */

static const struct graph_event *replay_events;
static size_t replay_count, replay_pos;
static bool   replay_active;

int rk_graph_replay_begin(const struct graph_event *events, size_t count)
{
	if (!events || !count)
		return RK_EINVAL;
	replay_events = events;
	replay_count  = count;
	replay_pos    = 0;
	replay_active = true;
	rk_time_enter_replay(events[0].mono_ns, rk_unix_time());
	gstats.replays++;
	pr_notice("replay started: %llu events", (unsigned long long)count);
	return RK_OK;
}

int rk_graph_replay_step(void)
{
	if (!replay_active || replay_pos >= replay_count)
		return RK_ENOENT;

	const struct graph_event *e = &replay_events[replay_pos++];
	rk_time_replay_advance(e->mono_ns);

	/* Only the events that describe state are re-applied. Scheduling and
	 * interrupt events are advisory during replay: they establish the
	 * timeline, they do not re-drive the hardware. */
	switch (e->kind) {
	case GEV_NODE_CREATE:
	case GEV_NODE_UPDATE:
	case GEV_NODE_DESTROY:
	case GEV_EDGE_ADD:
	case GEV_EDGE_DEL:
		break;
	default:
		break;
	}
	return replay_pos < replay_count ? RK_OK : RK_ENOENT;
}

void rk_graph_replay_end(void)
{
	replay_active = false;
	rk_time_leave_replay();
}

bool rk_graph_replaying(void) { return replay_active; }

void rk_graph_stats(struct graph_stats *out) { *out = gstats; }

/* ------------------------------------------------------------------ init */

void rk_graph_init(void)
{
	spin_lock_init(&graph_lock, "graph");
	node_cache  = kmem_cache_create("graph_node", sizeof(struct graph_node), 64);
	edge_cache  = kmem_cache_create("graph_edge", sizeof(struct graph_edge), 32);
	index_cache = kmem_cache_create("graph_index", sizeof(struct node_index), 16);
	if (!node_cache || !edge_cache || !index_cache)
		panic("graph: cannot allocate object caches");

	for (size_t i = 0; i < NODE_HASH_BUCKETS; i++)
		list_init(&node_hash[i]);
	list_init(&all_nodes);

	/* Recording defaults: structure and causality on, the high-rate hardware
	 * events off. A user who wants a full trace turns them on; a user who
	 * does not should not pay for one. */
	for (int i = 0; i < GEV_KIND_COUNT; i++)
		event_enabled[i] = true;
	event_enabled[GEV_SCHED_SWITCH] = false;
	event_enabled[GEV_IRQ] = false;
	event_enabled[GEV_CAP_CHECK] = false;

	root = kmem_cache_alloc(node_cache);
	if (!root)
		panic("graph: cannot allocate root");
	memset(root, 0, sizeof(*root));
	root->id = __atomic_add_fetch(&next_node_id, 1, __ATOMIC_SEQ_CST) - 1;
	root->kind = GNODE_ROOT;
	root->flags = GNODE_F_PINNED;
	strlcpy(root->label, "resentment", sizeof(root->label));
	list_init(&root->children);
	list_init(&root->sibling);
	list_init(&root->edges_out);
	list_init(&root->edges_in);
	spin_lock_init(&root->lock, "graph-root");
	index_insert(root);
	gstats.nodes = 1;

	graph_ready = true;
	pr_info("runtime graph online, root node %llu", (unsigned long long)root->id);
}
