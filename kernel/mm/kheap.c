/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - kernel heap.
 *
 * Slab caches for the size classes the kernel actually allocates, and direct
 * page allocation above that. Slabs because the kernel allocates the same few
 * object sizes over and over - a thread, a vnode, a graph node, a capability -
 * and a general purpose allocator spends its life re-deriving that fact.
 *
 * Every kmalloc carries a 16-byte header. It costs memory, and it buys the
 * ability to free without being told the size, to detect a double free, and to
 * answer ksize honestly. In a kernel that ships a scripting language and a
 * tensor runtime, both of which allocate on behalf of untrusted callers, that
 * trade is not close.
 */
#include <rk/mm.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/panic.h>
#include <rk/errno.h>
#include <rk/spinlock.h>
#include <rk/printf.h>
#include <rk/arch.h>

#undef RK_SUBSYS
#define RK_SUBSYS "kheap"

#define KH_MAGIC      0x4B48ADDEu
#define KH_MAGIC_FREE 0xDEADBEEFu

#define KH_KIND_SLAB 0
#define KH_KIND_PAGE 1

struct kh_hdr {
	u32 magic;
	u16 kind;
	u16 idx;        /* slab class index, or page order */
	u32 offset;     /* bytes from the real allocation base to this header */
	u32 size;       /* size the caller asked for */
};

RK_STATIC_ASSERT(sizeof(struct kh_hdr) == 16, "heap header must keep 16-byte alignment");

/* ------------------------------------------------------------- slab layer */

struct slab {
	struct list_head   link;
	struct kmem_cache *cache;
	void              *freelist;   /* singly linked through the first word */
	u32                inuse;
	u32                total;
	unsigned           order;
};

struct kmem_cache {
	char             name[24];
	size_t           objsize;
	size_t           align;
	unsigned         order;        /* pages per slab */
	u32              per_slab;
	struct list_head partial;
	struct list_head full;
	struct list_head empty;
	spinlock_t       lock;
	u64              allocs, frees, slabs;
	struct list_head link;
};

static LIST_HEAD(all_caches);
static struct kmem_cache cache_pool[32];
static u32 cache_pool_used;
static struct kheap_stats hstats;
static DEFINE_SPINLOCK(hstats_lock);

static struct slab *slab_new(struct kmem_cache *c)
{
	paddr_t pa = pmm_alloc_pages(c->order, 0);
	if (!pa)
		return NULL;

	struct slab *s = (struct slab *)arch_phys_to_virt(pa);
	size_t bytes = (size_t)RK_PAGE_SIZE << c->order;
	size_t hdr   = ALIGN_UP(sizeof(struct slab), c->align);

	s->cache = c;
	s->inuse = 0;
	s->order = c->order;
	s->total = (u32)((bytes - hdr) / c->objsize);
	s->freelist = NULL;

	/* Thread the free list through the objects themselves; a separate index
	 * would double the metadata for no benefit. */
	u8 *base = (u8 *)s + hdr;
	for (u32 i = s->total; i-- > 0;) {
		void *obj = base + (size_t)i * c->objsize;
		*(void **)obj = s->freelist;
		s->freelist = obj;
	}

	struct page *pg = pmm_page_of(pa);
	if (pg) {
		pg->flags |= RK_PG_SLAB;
		pg->owner = s;
	}
	c->slabs++;

	unsigned long f = spin_lock_irqsave(&hstats_lock);
	hstats.slab_pages += 1ull << c->order;
	spin_unlock_irqrestore(&hstats_lock, f);
	return s;
}

/* Find the slab an object belongs to. Slabs are order-aligned page blocks, so
 * masking the address down to the block start finds the header without a
 * lookup table.
 *
 * This depends on the direct map preserving alignment: the buddy allocator
 * returns order-aligned physical blocks, and arch_phys_to_virt must not
 * destroy that. Every supported architecture maps physical zero at a 2 MiB
 * aligned virtual address, which is more than enough. */
static struct slab *slab_of(void *obj, unsigned order)
{
	uintptr_t mask = ((uintptr_t)RK_PAGE_SIZE << order) - 1;
	return (struct slab *)((uintptr_t)obj & ~mask);
}

struct kmem_cache *kmem_cache_create(const char *name, size_t objsize, size_t align)
{
	if (cache_pool_used >= ARRAY_SIZE(cache_pool))
		return NULL;
	if (align < 16)
		align = 16;

	struct kmem_cache *c = &cache_pool[cache_pool_used++];
	strlcpy(c->name, name, sizeof(c->name));
	c->align   = align;
	c->objsize = ALIGN_UP(MAX(objsize, sizeof(void *)), align);

	/* Choose a slab size that wastes less than about 12% on the header and
	 * the tail remainder. Large objects get their own multi-page slab. */
	c->order = 0;
	while (c->order < 4) {
		size_t bytes = (size_t)RK_PAGE_SIZE << c->order;
		size_t hdr   = ALIGN_UP(sizeof(struct slab), align);
		if (bytes > hdr && (bytes - hdr) / c->objsize >= 8)
			break;
		c->order++;
	}
	size_t bytes = (size_t)RK_PAGE_SIZE << c->order;
	size_t hdr   = ALIGN_UP(sizeof(struct slab), align);
	c->per_slab  = (u32)((bytes - hdr) / c->objsize);
	if (!c->per_slab)
		c->per_slab = 1;

	list_init(&c->partial);
	list_init(&c->full);
	list_init(&c->empty);
	spin_lock_init(&c->lock, c->name);
	list_add_tail(&c->link, &all_caches);
	return c;
}

void *kmem_cache_alloc(struct kmem_cache *c)
{
	if (!c)
		return NULL;

	unsigned long f = spin_lock_irqsave(&c->lock);
	struct slab *s = NULL;

	if (!list_empty(&c->partial))
		s = list_first_entry(&c->partial, struct slab, link);
	else if (!list_empty(&c->empty))
		s = list_first_entry(&c->empty, struct slab, link);

	if (!s) {
		spin_unlock_irqrestore(&c->lock, f);
		s = slab_new(c);
		if (!s)
			return NULL;
		f = spin_lock_irqsave(&c->lock);
		list_add(&s->link, &c->partial);
	} else {
		list_move_tail(&s->link, &c->partial);
	}

	void *obj = s->freelist;
	s->freelist = *(void **)obj;
	s->inuse++;
	if (!s->freelist)
		list_move_tail(&s->link, &c->full);
	c->allocs++;
	spin_unlock_irqrestore(&c->lock, f);
	return obj;
}

void kmem_cache_free(struct kmem_cache *c, void *obj)
{
	if (!c || !obj)
		return;

	struct slab *s = slab_of(obj, c->order);
	unsigned long f = spin_lock_irqsave(&c->lock);

	*(void **)obj = s->freelist;
	s->freelist = obj;
	s->inuse--;
	c->frees++;

	if (s->inuse == 0)
		list_move_tail(&s->link, &c->empty);
	else
		list_move_tail(&s->link, &c->partial);
	spin_unlock_irqrestore(&c->lock, f);
}

void kmem_cache_destroy(struct kmem_cache *c)
{
	if (!c)
		return;
	struct slab *s, *tmp;
	unsigned long f = spin_lock_irqsave(&c->lock);
	list_for_each_entry_safe(s, tmp, &c->empty, link) {
		list_del(&s->link);
		pmm_free_pages(arch_virt_to_phys((vaddr_t)s), s->order);
	}
	spin_unlock_irqrestore(&c->lock, f);
	list_del(&c->link);
}

/* ---------------------------------------------------------------- kmalloc */

/* Powers of two from 32 to 4096 bytes of payload. Below 32 the header
 * dominates; above 4096 a page allocation is cheaper than a slab. */
static const size_t size_class[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096 };
#define NCLASSES ARRAY_SIZE(size_class)

static struct kmem_cache *kmalloc_cache[NCLASSES];
static bool kheap_ready;

void kheap_init(void)
{
	static const char *const names[NCLASSES] = {
		"kmalloc-32", "kmalloc-64", "kmalloc-128", "kmalloc-256",
		"kmalloc-512", "kmalloc-1k", "kmalloc-2k", "kmalloc-4k"
	};
	for (size_t i = 0; i < NCLASSES; i++)
		kmalloc_cache[i] = kmem_cache_create(names[i], size_class[i] + sizeof(struct kh_hdr), 16);
	kheap_ready = true;
	pr_info("kernel heap: %u slab classes, 32 B to 4 KiB", (unsigned)NCLASSES);
}

static int class_for(size_t n)
{
	for (size_t i = 0; i < NCLASSES; i++)
		if (n <= size_class[i])
			return (int)i;
	return -1;
}

static void account(s64 delta, bool alloc)
{
	unsigned long f = spin_lock_irqsave(&hstats_lock);
	if (delta > 0)
		hstats.bytes_live += (u64)delta;
	else
		hstats.bytes_live -= (u64)(-delta);
	if (hstats.bytes_live > hstats.bytes_peak)
		hstats.bytes_peak = hstats.bytes_live;
	if (alloc)
		hstats.allocs++;
	else
		hstats.frees++;
	spin_unlock_irqrestore(&hstats_lock, f);
}

void *kmalloc_aligned(size_t n, size_t align)
{
	if (!n || n > (1ull << 40))
		return NULL;
	if (align < 16)
		align = 16;
	RK_ASSERT_MSG((align & (align - 1)) == 0, "kmalloc alignment must be a power of two");

	/* The common case: 16-byte alignment out of a slab. */
	if (align <= 16) {
		int cls = class_for(n);
		if (cls >= 0 && kheap_ready) {
			struct kh_hdr *h = kmem_cache_alloc(kmalloc_cache[cls]);
			if (!h)
				return NULL;
			h->magic  = KH_MAGIC;
			h->kind   = KH_KIND_SLAB;
			h->idx    = (u16)cls;
			h->offset = 0;
			h->size   = (u32)n;
			account((s64)n, true);
			return h + 1;
		}
	}

	/* Anything larger or more strictly aligned comes from the page allocator
	 * with enough slack to place the header immediately before the aligned
	 * payload, whatever the allocation happened to land on. */
	size_t need = n + sizeof(struct kh_hdr) + align;
	unsigned order = 0;
	while (((size_t)RK_PAGE_SIZE << order) < need && order < RK_PMM_MAX_ORDER)
		order++;
	if (((size_t)RK_PAGE_SIZE << order) < need)
		return NULL;

	paddr_t pa = pmm_alloc_pages(order, 0);
	if (!pa)
		return NULL;

	u8 *base = (u8 *)arch_phys_to_virt(pa);
	u8 *payload = (u8 *)ALIGN_UP((uintptr_t)(base + sizeof(struct kh_hdr)), align);
	struct kh_hdr *h = (struct kh_hdr *)payload - 1;
	h->magic  = KH_MAGIC;
	h->kind   = KH_KIND_PAGE;
	h->idx    = (u16)order;
	h->offset = (u32)((u8 *)h - base);
	h->size   = (u32)n;

	unsigned long f = spin_lock_irqsave(&hstats_lock);
	hstats.large_pages += 1ull << order;
	spin_unlock_irqrestore(&hstats_lock, f);
	account((s64)n, true);
	return payload;
}

void *kmalloc(size_t n) { return kmalloc_aligned(n, 16); }

void *kzalloc(size_t n)
{
	void *p = kmalloc(n);
	if (p)
		memset(p, 0, n);
	return p;
}

void *kcalloc(size_t count, size_t size)
{
	/* Overflow here is a classic heap corruption primitive; refuse instead. */
	if (size && count > (size_t)-1 / size)
		return NULL;
	return kzalloc(count * size);
}

size_t ksize(const void *p)
{
	if (!p)
		return 0;
	const struct kh_hdr *h = (const struct kh_hdr *)p - 1;
	return h->magic == KH_MAGIC ? h->size : 0;
}

void kfree(void *p)
{
	if (!p)
		return;

	struct kh_hdr *h = (struct kh_hdr *)p - 1;
	if (h->magic == KH_MAGIC_FREE) {
		pr_err("double free at %p", p);
		return;
	}
	if (h->magic != KH_MAGIC) {
		pr_err("kfree of %p with bad header %#x", p, h->magic);
		return;
	}

	u32 size = h->size;
	u16 kind = h->kind, idx = h->idx;
	u32 offset = h->offset;
	h->magic = KH_MAGIC_FREE;

	if (kind == KH_KIND_SLAB) {
		kmem_cache_free(kmalloc_cache[idx], h);
	} else {
		u8 *base = (u8 *)h - offset;
		unsigned long f = spin_lock_irqsave(&hstats_lock);
		hstats.large_pages -= 1ull << idx;
		spin_unlock_irqrestore(&hstats_lock, f);
		pmm_free_pages(arch_virt_to_phys((vaddr_t)base), idx);
	}
	account(-(s64)size, false);
}

void *krealloc(void *p, size_t n)
{
	if (!p)
		return kmalloc(n);
	if (!n) {
		kfree(p);
		return NULL;
	}
	size_t old = ksize(p);
	if (n <= old)
		return p;   /* shrinking in place is free and callers expect it */

	void *q = kmalloc(n);
	if (!q)
		return NULL;
	memcpy(q, p, old);
	kfree(p);
	return q;
}

char *kstrdup(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s) + 1;
	char *d = kmalloc(n);
	if (d)
		memcpy(d, s, n);
	return d;
}

void *kmemdup(const void *p, size_t n)
{
	void *d = kmalloc(n);
	if (d)
		memcpy(d, p, n);
	return d;
}

void kheap_stats(struct kheap_stats *out)
{
	unsigned long f = spin_lock_irqsave(&hstats_lock);
	*out = hstats;
	spin_unlock_irqrestore(&hstats_lock, f);
}

/* Used by the shell to show where kernel memory actually went. */
size_t kheap_dump(char *buf, size_t cap)
{
	size_t n = 0;
	struct kmem_cache *c;
	n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
	                      "%-14s %8s %8s %8s %8s\n",
	                      "cache", "objsize", "slabs", "allocs", "frees");
	list_for_each_entry(c, &all_caches, link)
		n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
		                      "%-14s %8llu %8llu %8llu %8llu\n", c->name,
		                      (unsigned long long)c->objsize,
		                      (unsigned long long)c->slabs,
		                      (unsigned long long)c->allocs,
		                      (unsigned long long)c->frees);
	return n;
}
