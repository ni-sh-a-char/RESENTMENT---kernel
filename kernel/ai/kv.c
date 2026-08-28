/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - paged attention cache.
 *
 * The observation this is built on: an attention cache behaves exactly like
 * memory. It is allocated in blocks, most of it is cold, sequences that share
 * a prefix hold identical contents, and the worst case is far larger than the
 * common case. Every one of those facts is something a kernel memory manager
 * already knows how to exploit, and a userspace runtime cannot - which is why
 * per-process cache reservation wastes most of the RAM on a machine running
 * several models.
 *
 * So: pages of a fixed number of tokens, content-addressed by the digest of
 * the token prefix they cover, refcounted and shared between sequences, and
 * evicted least-recently-used under pressure instead of pre-reserved.
 *
 * Two sessions with the same system prompt store it once. That is not a micro
 * optimisation; on a machine serving several conversations it is most of the
 * cache.
 */
#include <rk/ai.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/time.h>
#include <rk/crypto.h>
#include <rk/graph.h>

#undef RK_SUBSYS
#define RK_SUBSYS "kv"

#define KV_HASH_BUCKETS 256

static struct list_head page_hash[KV_HASH_BUCKETS];
static LIST_HEAD(kv_lru);
static struct mutex kv_lock;
static rk_id_t next_kv_id = 1;
static u64 kv_pages_live, kv_pages_shared, kv_bytes_live, kv_evictions;
static u64 kv_budget = 64ull << 20;   /* 64 MiB by default, tunable at boot */

static size_t page_bytes(const struct rk_kv_cache *c)
{
	size_t elem = rk_dtype_size((enum rk_dtype)c->dtype);
	if (!elem)
		elem = 2;
	return (size_t)RK_KV_PAGE_TOKENS * c->n_kv_heads * c->head_dim * elem;
}

static size_t bucket_of(const u8 digest[SHA256_DIGEST_SIZE])
{
	return ((size_t)digest[0] << 8 | digest[1]) % KV_HASH_BUCKETS;
}

static struct rk_kv_page *page_lookup(const u8 digest[SHA256_DIGEST_SIZE], u16 layer)
{
	struct rk_kv_page *p;
	list_for_each_entry(p, &page_hash[bucket_of(digest)], hash)
		if (p->layer == layer &&
		    memcmp(p->digest, digest, SHA256_DIGEST_SIZE) == 0)
			return p;
	return NULL;
}

static void evict_locked(u64 want)
{
	struct rk_kv_page *p, *tmp;
	list_for_each_entry_safe(p, tmp, &kv_lru, lru) {
		if (kv_bytes_live + want <= kv_budget)
			break;
		if (p->refcount)
			continue;   /* still referenced by a live sequence */
		list_del(&p->lru);
		list_del(&p->hash);
		kfree(p->k);
		kfree(p->v);
		kv_bytes_live -= p->bytes;
		kv_pages_live--;
		kv_evictions++;
		kfree(p);
	}
}

struct rk_kv_cache *rk_kv_create(rk_id_t seq_id, u32 layers, u32 kv_heads,
                                 u32 head_dim, enum rk_dtype dt, u32 max_tokens)
{
	if (!layers || !kv_heads || !head_dim)
		return NULL;

	struct rk_kv_cache *c = kzalloc(sizeof(*c));
	if (!c)
		return NULL;

	c->id         = __atomic_add_fetch(&next_kv_id, 1, __ATOMIC_SEQ_CST) - 1;
	c->seq_id     = seq_id;
	c->n_layers   = layers;
	c->n_kv_heads = kv_heads;
	c->head_dim   = head_dim;
	c->dtype      = (u8)dt;
	c->max_pages  = layers * DIV_ROUND_UP(max_tokens ? max_tokens : 2048,
	                                      RK_KV_PAGE_TOKENS);
	mutex_init(&c->lock, "kvcache");

	c->pages = kcalloc(c->max_pages, sizeof(struct rk_kv_page *));
	if (!c->pages) {
		kfree(c);
		return NULL;
	}

	struct graph_node *n = rk_graph_node_create(GNODE_TENSOR, "kvcache", NULL, c);
	if (n) {
		rk_graph_set_u64(n, "seq", seq_id);
		rk_graph_set_u64(n, "layers", layers);
		c->graph_node = n->id;
	}
	return c;
}

void rk_kv_destroy(struct rk_kv_cache *c)
{
	if (!c)
		return;

	mutex_lock(&kv_lock);
	for (u32 i = 0; i < c->npages; i++) {
		struct rk_kv_page *p = c->pages[i];
		if (!p)
			continue;
		if (p->refcount)
			p->refcount--;
		if (p->refcount == 0 && kv_pages_shared)
			kv_pages_shared--;
	}
	mutex_unlock(&kv_lock);

	kfree(c->pages);
	kfree(c);
}

/* Append a page of keys and values. If a page with the same content digest
 * already exists, the sequence adopts it instead of storing a second copy. */
int rk_kv_append(struct rk_kv_cache *c, u32 layer, const void *k, const void *v, u32 ntok)
{
	if (!c || !k || !v || layer >= c->n_layers || !ntok || ntok > RK_KV_PAGE_TOKENS)
		return RK_EINVAL;
	if (c->npages >= c->max_pages)
		return RK_ENOSPC;

	size_t bytes = page_bytes(c);

	/* The digest covers the layer, the position and the contents, so two
	 * sequences only share a page when they genuinely agree on all three. */
	u8 digest[SHA256_DIGEST_SIZE];
	struct sha256_ctx sc;
	sha256_init(&sc);
	sha256_update(&sc, &layer, sizeof(layer));
	sha256_update(&sc, &c->tokens, sizeof(c->tokens));
	sha256_update(&sc, k, bytes);
	sha256_update(&sc, v, bytes);
	sha256_final(&sc, digest);

	mutex_lock(&kv_lock);

	struct rk_kv_page *p = page_lookup(digest, (u16)layer);
	if (p) {
		p->refcount++;
		p->last_used_ns = rk_time_ns();
		list_move_tail(&p->lru, &kv_lru);
		kv_pages_shared++;
		c->pages[c->npages++] = p;
		c->tokens += ntok;
		mutex_unlock(&kv_lock);
		return RK_OK;
	}

	evict_locked(bytes * 2);

	p = kzalloc(sizeof(*p));
	if (!p) {
		mutex_unlock(&kv_lock);
		return RK_ENOMEM;
	}
	p->k = kmalloc(bytes);
	p->v = kmalloc(bytes);
	if (!p->k || !p->v) {
		kfree(p->k);
		kfree(p->v);
		kfree(p);
		mutex_unlock(&kv_lock);
		return RK_ENOMEM;
	}
	memcpy(p->k, k, bytes);
	memcpy(p->v, v, bytes);
	memcpy(p->digest, digest, sizeof(digest));
	p->layer    = (u16)layer;
	p->ntokens  = (u16)ntok;
	p->refcount = 1;
	p->bytes    = (u32)(bytes * 2);
	p->id       = __atomic_add_fetch(&next_kv_id, 1, __ATOMIC_SEQ_CST) - 1;
	p->last_used_ns = rk_time_ns();

	list_add(&p->hash, &page_hash[bucket_of(digest)]);
	list_add_tail(&p->lru, &kv_lru);
	kv_pages_live++;
	kv_bytes_live += bytes * 2;

	c->pages[c->npages++] = p;
	c->tokens += ntok;
	mutex_unlock(&kv_lock);
	return RK_OK;
}

/* Fork a sequence: the new cache shares every page up to the branch point.
 * This is what makes speculative decoding and beam search affordable - a
 * branch costs a refcount, not a copy of the cache. */
int rk_kv_fork(struct rk_kv_cache *dst, struct rk_kv_cache *src, u32 upto_token)
{
	if (!dst || !src)
		return RK_EINVAL;

	u32 want_pages = DIV_ROUND_UP(upto_token, RK_KV_PAGE_TOKENS) * src->n_layers;
	if (want_pages > src->npages)
		want_pages = src->npages;
	if (want_pages > dst->max_pages)
		return RK_ENOSPC;

	mutex_lock(&kv_lock);
	for (u32 i = 0; i < want_pages; i++) {
		struct rk_kv_page *p = src->pages[i];
		if (!p)
			continue;
		p->refcount++;
		kv_pages_shared++;
		dst->pages[i] = p;
	}
	dst->npages = want_pages;
	dst->tokens = upto_token;
	mutex_unlock(&kv_lock);
	return RK_OK;
}

void rk_kv_trim(struct rk_kv_cache *c, u32 keep_tokens)
{
	if (!c)
		return;
	u32 keep_pages = DIV_ROUND_UP(keep_tokens, RK_KV_PAGE_TOKENS) * c->n_layers;
	if (keep_pages >= c->npages)
		return;

	mutex_lock(&kv_lock);
	for (u32 i = keep_pages; i < c->npages; i++) {
		struct rk_kv_page *p = c->pages[i];
		if (p && p->refcount)
			p->refcount--;
		c->pages[i] = NULL;
	}
	c->npages = keep_pages;
	c->tokens = keep_tokens;
	mutex_unlock(&kv_lock);
}

/* Called by the memory manager under pressure. Attention caches are the
 * largest reclaimable allocation on an inference machine, so they are the
 * first thing asked to give memory back rather than the last. */
void rk_kv_pressure_evict(u64 want_bytes)
{
	mutex_lock(&kv_lock);
	u64 old_budget = kv_budget;
	if (kv_budget > want_bytes)
		kv_budget -= want_bytes;
	evict_locked(0);
	kv_budget = old_budget;
	mutex_unlock(&kv_lock);
}

void rk_kv_stats(u64 *pages, u64 *shared, u64 *bytes, u64 *evictions)
{
	if (pages)     *pages = kv_pages_live;
	if (shared)    *shared = kv_pages_shared;
	if (bytes)     *bytes = kv_bytes_live;
	if (evictions) *evictions = kv_evictions;
}

void rk_kv_init(u64 budget_bytes)
{
	mutex_init(&kv_lock, "kv");
	for (size_t i = 0; i < KV_HASH_BUCKETS; i++)
		list_init(&page_hash[i]);
	if (budget_bytes)
		kv_budget = budget_bytes;
	pr_info("paged KV cache ready: %u tokens per page, %pB budget",
	        RK_KV_PAGE_TOKENS, RK_BYTES(kv_budget));
}
