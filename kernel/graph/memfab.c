/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the federated memory fabric.
 *
 * WebWeaveX merges execution histories across turns into a deterministic
 * key/value store. RESENTMENT applies the same idea across boots: an agent
 * running on this machine can leave state behind that survives a restart, and
 * two machines can merge their fabrics without a conflict resolution policy,
 * because every entry is content-addressed. Merging is idempotent by
 * construction: the same content always lands on the same key.
 *
 * Deliberately bounded. This is kernel memory, so the fabric has a hard cap
 * and evicts the least recently used entry rather than growing without limit.
 */
#include <rk/graph.h>
#include <rk/mm.h>
#include <rk/crypto.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/printf.h>
#include <rk/time.h>
#include <rk/sync.h>

#undef RK_SUBSYS
#define RK_SUBSYS "memfab"

#define MEMFAB_BUCKETS   256
#define MEMFAB_MAX_BYTES (2u << 20)   /* 2 MiB, adjustable via the cmdline */
#define MEMFAB_MAX_VALUE 65536

struct memfab_entry {
	struct list_head link;
	struct list_head lru;
	u8     key[RK_GRAPH_DIGEST_SIZE];
	u32    len;
	u64    last_used_ns;
	u64    hits;
	u8     value[];
};

static struct list_head buckets[MEMFAB_BUCKETS];
static LIST_HEAD(lru_list);
static struct mutex fab_lock;
static u64 fab_bytes, fab_entries, fab_hits, fab_misses;
static u64 fab_cap = MEMFAB_MAX_BYTES;
static bool fab_ready;
static struct graph_node *fab_node;

/* The fabric is part of system state, so it belongs in the graph: storing
 * something must move the root digest, or "the machine changed" and "the hash
 * changed" come apart and the digest stops meaning anything. */
static void publish(void)
{
	if (!fab_node)
		return;
	rk_graph_set_u64(fab_node, "entries", fab_entries);
	rk_graph_set_u64(fab_node, "bytes", fab_bytes);
}

static size_t bucket_of(const u8 key[RK_GRAPH_DIGEST_SIZE])
{
	return ((size_t)key[0] << 8 | key[1]) % MEMFAB_BUCKETS;
}

void rk_memfab_init(u64 cap_bytes)
{
	for (size_t i = 0; i < MEMFAB_BUCKETS; i++)
		list_init(&buckets[i]);
	mutex_init(&fab_lock, "memfab");
	if (cap_bytes)
		fab_cap = cap_bytes;
	fab_ready = true;
	fab_node = rk_graph_node_create(GNODE_MEMORY, "memfab", NULL, NULL);
	publish();
	pr_info("federated memory fabric ready, %pB budget", RK_BYTES(fab_cap));
}

static struct memfab_entry *lookup_locked(const u8 key[RK_GRAPH_DIGEST_SIZE])
{
	struct memfab_entry *e;
	list_for_each_entry(e, &buckets[bucket_of(key)], link)
		if (memcmp(e->key, key, RK_GRAPH_DIGEST_SIZE) == 0)
			return e;
	return NULL;
}

static void evict_locked(u64 want)
{
	while (fab_bytes + want > fab_cap && !list_empty(&lru_list)) {
		struct memfab_entry *victim =
			list_last_entry(&lru_list, struct memfab_entry, lru);
		list_del(&victim->lru);
		list_del(&victim->link);
		fab_bytes -= victim->len;
		fab_entries--;
		kfree(victim);
	}
}

int rk_memfab_put(const u8 key[RK_GRAPH_DIGEST_SIZE], const void *val, size_t len)
{
	if (!fab_ready)
		return RK_EAGAIN;
	if (len > MEMFAB_MAX_VALUE)
		return RK_E2BIG;

	mutex_lock(&fab_lock);

	struct memfab_entry *old = lookup_locked(key);
	if (old) {
		/* Content-addressed: the same key means the same value, so a repeat
		 * put is a no-op rather than an update. This is what makes merging
		 * two fabrics safe without a conflict policy. */
		list_move_tail(&old->lru, &lru_list);
		old->last_used_ns = rk_time_ns();
		mutex_unlock(&fab_lock);
		return RK_OK;
	}

	evict_locked(len);

	struct memfab_entry *e = kmalloc(sizeof(*e) + len);
	if (!e) {
		mutex_unlock(&fab_lock);
		return RK_ENOMEM;
	}
	memcpy(e->key, key, RK_GRAPH_DIGEST_SIZE);
	e->len = (u32)len;
	e->hits = 0;
	e->last_used_ns = rk_time_ns();
	memcpy(e->value, val, len);

	list_add(&e->link, &buckets[bucket_of(key)]);
	list_add(&e->lru, &lru_list);
	fab_bytes += len;
	fab_entries++;
	publish();

	mutex_unlock(&fab_lock);
	return RK_OK;
}

size_t rk_memfab_get(const u8 key[RK_GRAPH_DIGEST_SIZE], void *val, size_t cap)
{
	if (!fab_ready)
		return 0;

	mutex_lock(&fab_lock);
	struct memfab_entry *e = lookup_locked(key);
	size_t n = 0;
	if (e) {
		n = e->len < cap ? e->len : cap;
		memcpy(val, e->value, n);
		e->hits++;
		e->last_used_ns = rk_time_ns();
		list_move_tail(&e->lru, &lru_list);
		fab_hits++;
	} else {
		fab_misses++;
	}
	mutex_unlock(&fab_lock);
	return n;
}

/* Convenience for callers with a human-readable key: the digest of the string
 * is the key, so a name and its content both end up content-addressed. */
static void key_of_string(const char *s, u8 out[RK_GRAPH_DIGEST_SIZE])
{
	struct sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, "memfab/name/", 12);
	sha256_update(&c, s, strlen(s));
	sha256_final(&c, out);
}

int rk_memfab_put_str(const char *key, const void *val, size_t len)
{
	u8 k[RK_GRAPH_DIGEST_SIZE];
	key_of_string(key, k);
	return rk_memfab_put(k, val, len);
}

size_t rk_memfab_get_str(const char *key, void *val, size_t cap)
{
	u8 k[RK_GRAPH_DIGEST_SIZE];
	key_of_string(key, k);
	return rk_memfab_get(k, val, cap);
}

/* Wire format: a header, then repeated (key, length, bytes). Fixed-width
 * big-endian throughout so a fabric written on one architecture merges into
 * another without a byte-order negotiation. */
struct fab_header {
	u8  magic[8];      /* "RKMEMFAB" */
	u32 version;
	u32 count;
	u64 bytes;
};

size_t rk_memfab_serialize(void *out, size_t cap)
{
	mutex_lock(&fab_lock);

	size_t need = sizeof(struct fab_header);
	struct memfab_entry *e;
	list_for_each_entry(e, &lru_list, lru)
		need += RK_GRAPH_DIGEST_SIZE + 4 + e->len;

	if (cap < need) {
		mutex_unlock(&fab_lock);
		return need;   /* probe call: tell the caller how much to allocate */
	}

	u8 *p = out;
	struct fab_header h;
	memcpy(h.magic, "RKMEMFAB", 8);
	h.version = 1;
	h.count   = (u32)fab_entries;
	h.bytes   = fab_bytes;
	memcpy(p, &h, sizeof(h));
	p += sizeof(h);

	list_for_each_entry(e, &lru_list, lru) {
		memcpy(p, e->key, RK_GRAPH_DIGEST_SIZE);
		p += RK_GRAPH_DIGEST_SIZE;
		for (int i = 0; i < 4; i++)
			*p++ = (u8)(e->len >> (24 - i * 8));
		memcpy(p, e->value, e->len);
		p += e->len;
	}

	mutex_unlock(&fab_lock);
	return (size_t)(p - (u8 *)out);
}

int rk_memfab_merge(const void *blob, size_t len)
{
	if (len < sizeof(struct fab_header))
		return RK_EINVAL;

	const u8 *p = blob;
	const u8 *end = p + len;
	struct fab_header h;
	memcpy(&h, p, sizeof(h));
	if (memcmp(h.magic, "RKMEMFAB", 8) != 0 || h.version != 1)
		return RK_EINVAL;
	p += sizeof(h);

	u32 merged = 0;
	while (p + RK_GRAPH_DIGEST_SIZE + 4 <= end) {
		const u8 *key = p;
		p += RK_GRAPH_DIGEST_SIZE;
		u32 vlen = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
		p += 4;
		if (vlen > MEMFAB_MAX_VALUE || p + vlen > end)
			return RK_EINVAL;
		if (rk_memfab_put(key, p, vlen) == RK_OK)
			merged++;
		p += vlen;
	}
	pr_info("merged %u fabric entries", merged);
	return RK_OK;
}

void rk_memfab_stats(u64 *entries, u64 *bytes, u64 *hits, u64 *misses)
{
	if (entries) *entries = fab_entries;
	if (bytes)   *bytes   = fab_bytes;
	if (hits)    *hits    = fab_hits;
	if (misses)  *misses  = fab_misses;
}
