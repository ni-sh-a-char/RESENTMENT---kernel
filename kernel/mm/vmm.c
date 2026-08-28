/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - virtual memory.
 *
 * Address spaces are lists of areas, each area a window onto a vm_object.
 * Making the object the unit of storage rather than the mapping is what lets
 * fork, shared memory, file mapping and zero-copy IPC all be the same code
 * path: they differ only in who else holds a reference to the object.
 *
 * Pages are allocated on fault, not on map. A tensor arena of a gigabyte costs
 * nothing until it is touched, which matters when a model declares its working
 * set up front and then uses a fraction of it.
 */
#include <rk/mm.h>
#include <rk/arch.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/panic.h>
#include <rk/printf.h>
#include <rk/sched.h>
#include <rk/graph.h>

#undef RK_SUBSYS
#define RK_SUBSYS "vmm"

#define USER_MIN      0x0000000000400000ull
#define USER_MAX      0x00007FFFFFFFF000ull
#define USER_MMAP_BASE 0x0000200000000000ull

static struct address_space kernel_as;
static struct kmem_cache *as_cache;
static struct kmem_cache *area_cache;
static struct kmem_cache *obj_cache;
static rk_id_t next_as_id = 1;

/* --------------------------------------------------------------- objects */

struct vm_object *vm_object_anon(size_t npages, const char *name)
{
	struct vm_object *o = kmem_cache_alloc(obj_cache);
	if (!o)
		return NULL;
	memset(o, 0, sizeof(*o));

	o->kind   = VM_OBJ_ANON;
	o->npages = npages;
	o->frames = kcalloc(npages, sizeof(paddr_t));
	if (!o->frames) {
		kmem_cache_free(obj_cache, o);
		return NULL;
	}
	o->refcount = 1;
	o->name = name;
	spin_lock_init(&o->lock, "vmobj");
	return o;
}

struct vm_object *vm_object_phys(paddr_t base, size_t npages, const char *name)
{
	struct vm_object *o = kmem_cache_alloc(obj_cache);
	if (!o)
		return NULL;
	memset(o, 0, sizeof(*o));
	o->kind      = VM_OBJ_PHYS;
	o->npages    = npages;
	o->phys_base = base;
	o->refcount  = 1;
	o->name      = name;
	spin_lock_init(&o->lock, "vmobj-phys");
	return o;
}

void vm_object_get(struct vm_object *o)
{
	if (!o)
		return;
	unsigned long f = spin_lock_irqsave(&o->lock);
	o->refcount++;
	spin_unlock_irqrestore(&o->lock, f);
}

void vm_object_put(struct vm_object *o)
{
	if (!o)
		return;
	unsigned long f = spin_lock_irqsave(&o->lock);
	bool last = (--o->refcount == 0);
	spin_unlock_irqrestore(&o->lock, f);
	if (!last)
		return;

	if (o->kind == VM_OBJ_ANON && o->frames) {
		for (size_t i = 0; i < o->npages; i++)
			if (o->frames[i])
				pmm_page_put(o->frames[i]);
		kfree(o->frames);
	}
	kmem_cache_free(obj_cache, o);
}

/* Resolve a page of an object, allocating it if this is the first touch. */
static paddr_t vm_object_page(struct vm_object *o, size_t index)
{
	if (index >= o->npages)
		return 0;
	if (o->kind == VM_OBJ_PHYS)
		return o->phys_base + ((paddr_t)index << RK_PAGE_SHIFT);

	unsigned long f = spin_lock_irqsave(&o->lock);
	paddr_t pa = o->frames[index];
	if (!pa) {
		pa = pmm_alloc_page();
		if (pa)
			o->frames[index] = pa;
	}
	spin_unlock_irqrestore(&o->lock, f);
	return pa;
}

/* ---------------------------------------------------------- address spaces */

struct address_space *as_kernel(void) { return &kernel_as; }

struct address_space *as_create(void)
{
	struct address_space *as = kmem_cache_alloc(as_cache);
	if (!as)
		return NULL;
	memset(as, 0, sizeof(*as));

	as->pgtable = arch_pgtable_create();
	if (!as->pgtable) {
		kmem_cache_free(as_cache, as);
		return NULL;
	}
	list_init(&as->areas);
	spin_lock_init(&as->lock, "as");
	as->refcount  = 1;
	as->mmap_base = USER_MMAP_BASE;
	as->brk       = USER_MIN;
	as->id        = next_as_id++;
	return as;
}

void as_destroy(struct address_space *as)
{
	if (!as || as == &kernel_as)
		return;

	unsigned long f = spin_lock_irqsave(&as->lock);
	if (--as->refcount > 0) {
		spin_unlock_irqrestore(&as->lock, f);
		return;
	}
	struct vm_area *a, *tmp;
	list_for_each_entry_safe(a, tmp, &as->areas, link) {
		list_del(&a->link);
		vm_object_put(a->obj);
		kmem_cache_free(area_cache, a);
	}
	spin_unlock_irqrestore(&as->lock, f);

	arch_pgtable_destroy(as->pgtable);
	kmem_cache_free(as_cache, as);
}

void as_switch(struct address_space *as)
{
	arch_pgtable_switch(as ? as->pgtable : kernel_as.pgtable);
}

struct vm_area *as_find(struct address_space *as, vaddr_t va)
{
	struct vm_area *a;
	list_for_each_entry(a, &as->areas, link)
		if (va >= a->start && va < a->end)
			return a;
	return NULL;
}

/* First-fit over the sorted area list. First-fit rather than best-fit because
 * a 47-bit address space does not need the packing, and the search is the part
 * that shows up in profiles. */
static vaddr_t find_gap(struct address_space *as, vaddr_t hint, size_t len)
{
	vaddr_t cursor = hint ? ALIGN_UP(hint, RK_PAGE_SIZE) : as->mmap_base;
	bool restart = true;

	while (restart) {
		restart = false;
		struct vm_area *a;
		list_for_each_entry(a, &as->areas, link) {
			if (cursor + len > a->start && cursor < a->end) {
				cursor = ALIGN_UP(a->end, RK_PAGE_SIZE);
				restart = true;
				break;
			}
		}
		if (cursor + len > USER_MAX && as != &kernel_as)
			return 0;
	}
	return cursor;
}

static void insert_area(struct address_space *as, struct vm_area *n)
{
	struct vm_area *a;
	list_for_each_entry(a, &as->areas, link) {
		if (n->start < a->start) {
			__list_add(&n->link, a->link.prev, &a->link);
			return;
		}
	}
	list_add_tail(&n->link, &as->areas);
}

int as_map_object(struct address_space *as, vaddr_t hint, struct vm_object *obj,
                  size_t obj_offset, size_t npages, u32 prot, u32 flags,
                  const char *name, vaddr_t *out)
{
	if (!as || !obj || !npages)
		return RK_EINVAL;

	size_t len = npages << RK_PAGE_SHIFT;
	unsigned long f = spin_lock_irqsave(&as->lock);

	vaddr_t va;
	if (flags & VM_AREA_FIXED) {
		va = ALIGN_DOWN(hint, RK_PAGE_SIZE);
		if (as_find(as, va)) {
			spin_unlock_irqrestore(&as->lock, f);
			return RK_EEXIST;
		}
	} else {
		va = find_gap(as, hint, len);
		if (!va) {
			spin_unlock_irqrestore(&as->lock, f);
			return RK_ENOMEM;
		}
	}

	struct vm_area *a = kmem_cache_alloc(area_cache);
	if (!a) {
		spin_unlock_irqrestore(&as->lock, f);
		return RK_ENOMEM;
	}
	memset(a, 0, sizeof(*a));
	a->start = va;
	a->end   = va + len;
	a->prot  = prot;
	a->flags = flags;
	a->obj   = obj;
	a->obj_offset = obj_offset;
	a->name  = name;
	vm_object_get(obj);
	insert_area(as, a);
	spin_unlock_irqrestore(&as->lock, f);

	/* Device memory and locked areas are populated eagerly: a fault on a DMA
	 * buffer while a device is writing into it is not recoverable. */
	if (obj->kind == VM_OBJ_PHYS || (flags & VM_AREA_LOCKED)) {
		for (size_t i = 0; i < npages; i++) {
			paddr_t pa = vm_object_page(obj, obj_offset + i);
			if (!pa)
				return RK_ENOMEM;
			int r = arch_map(as->pgtable, va + (i << RK_PAGE_SHIFT), pa,
			                 RK_PAGE_SIZE, prot);
			if (r != RK_OK)
				return r;
		}
		as->rss_pages += npages;
	}

	if (out)
		*out = va;
	rk_graph_record(GEV_NODE_UPDATE, as->id, va, len, prot);
	return RK_OK;
}

int as_map_anon(struct address_space *as, vaddr_t hint, size_t len, u32 prot,
                const char *name, vaddr_t *out)
{
	size_t npages = DIV_ROUND_UP(len, RK_PAGE_SIZE);
	struct vm_object *o = vm_object_anon(npages, name);
	if (!o)
		return RK_ENOMEM;

	int r = as_map_object(as, hint, o, 0, npages, prot, 0, name, out);
	vm_object_put(o);   /* the area holds the reference now */
	return r;
}

int as_unmap(struct address_space *as, vaddr_t va, size_t len)
{
	unsigned long f = spin_lock_irqsave(&as->lock);
	struct vm_area *a, *tmp;
	vaddr_t end = va + len;

	list_for_each_entry_safe(a, tmp, &as->areas, link) {
		if (a->end <= va || a->start >= end)
			continue;
		/* Partial unmaps are not supported; a caller that needs one should
		 * map in the granularity it wants to unmap. Saying so is better than
		 * silently unmapping more than asked. */
		if (a->start < va || a->end > end) {
			spin_unlock_irqrestore(&as->lock, f);
			return RK_ENOTSUP;
		}
		arch_unmap(as->pgtable, a->start, a->end - a->start);
		as->rss_pages -= (a->end - a->start) >> RK_PAGE_SHIFT;
		list_del(&a->link);
		vm_object_put(a->obj);
		kmem_cache_free(area_cache, a);
	}
	spin_unlock_irqrestore(&as->lock, f);
	return RK_OK;
}

int as_protect(struct address_space *as, vaddr_t va, size_t len, u32 prot)
{
	unsigned long f = spin_lock_irqsave(&as->lock);
	struct vm_area *a = as_find(as, va);
	if (a)
		a->prot = prot;
	spin_unlock_irqrestore(&as->lock, f);
	return arch_protect(as->pgtable, va, len, prot);
}

/* Copy-on-write clone. Both sides lose write permission; the first write
 * faults, copies one page, and restores it. */
struct address_space *as_clone(struct address_space *src)
{
	struct address_space *dst = as_create();
	if (!dst)
		return NULL;

	unsigned long f = spin_lock_irqsave(&src->lock);
	struct vm_area *a;
	list_for_each_entry(a, &src->areas, link) {
		struct vm_area *n = kmem_cache_alloc(area_cache);
		if (!n)
			break;
		*n = *a;
		list_init(&n->link);
		vm_object_get(a->obj);
		insert_area(dst, n);

		if (a->flags & VM_AREA_SHARED)
			continue;

		u32 ro = a->prot & (u32)~RK_PROT_WRITE;
		n->prot = ro;
		arch_protect(src->pgtable, a->start, a->end - a->start, ro);
	}
	spin_unlock_irqrestore(&src->lock, f);
	return dst;
}

/* ----------------------------------------------------------------- faults */

/* ponytail: one lock for the whole address space during a fault. Correct and
 * simple; if concurrent faults in one process ever show up in a profile, split
 * it per-area or make the COW path use a per-object lock instead. */
int vm_handle_fault(struct address_space *as, vaddr_t va, u32 access)
{
	if (!as)
		return RK_EFAULT;

	unsigned long lf = spin_lock_irqsave(&as->lock);
	int rc = RK_EFAULT;
	vaddr_t page = ALIGN_DOWN(va, RK_PAGE_SIZE);
	struct vm_area *a = as_find(as, page);

	/* A stack area grows down: a fault just below it is legitimate, up to a
	 * limit, and anything further is a wild pointer rather than growth. */
	if (!a) {
		struct vm_area *cand;
		list_for_each_entry(cand, &as->areas, link) {
			if ((cand->flags & VM_AREA_GROWS) && page < cand->start &&
			    cand->start - page <= (8ull << 20)) {
				cand->start = page;
				a = cand;
				break;
			}
		}
	}
	if (!a)
		goto out;

	if ((access & VM_FAULT_WRITE) && !(a->prot & RK_PROT_WRITE)) {
		/* Either a copy-on-write page or a genuine permission violation. The
		 * object refcount tells them apart: shared means copy, sole owner
		 * means the mapping really is read only. */
		if (a->obj->refcount <= 1) {
			rc = RK_EACCES;
			goto out;
		}

		size_t cow_index = a->obj_offset + ((page - a->start) >> RK_PAGE_SHIFT);
		paddr_t old = vm_object_page(a->obj, cow_index);
		paddr_t fresh = pmm_alloc_page();
		if (!fresh) {
			rc = RK_ENOMEM;
			goto out;
		}
		memcpy((void *)arch_phys_to_virt(fresh), (void *)arch_phys_to_virt(old),
		       RK_PAGE_SIZE);

		struct vm_object *priv = vm_object_anon(a->obj->npages, "cow");
		if (!priv) {
			pmm_free_page(fresh);
			rc = RK_ENOMEM;
			goto out;
		}
		priv->frames[cow_index] = fresh;
		vm_object_put(a->obj);
		a->obj = priv;
		a->prot |= RK_PROT_WRITE;
		arch_map(as->pgtable, page, fresh, RK_PAGE_SIZE, a->prot);
		rc = RK_OK;
		goto out;
	}

	if (((access & VM_FAULT_EXEC) && !(a->prot & RK_PROT_EXEC)) ||
	    ((access & VM_FAULT_USER) && !(a->prot & RK_PROT_USER))) {
		rc = RK_EACCES;
		goto out;
	}

	size_t index = a->obj_offset + ((page - a->start) >> RK_PAGE_SHIFT);
	paddr_t pa = vm_object_page(a->obj, index);
	if (!pa) {
		rc = RK_ENOMEM;
		goto out;
	}

	rc = arch_map(as->pgtable, page, pa, RK_PAGE_SIZE, a->prot);
	if (rc == RK_OK)
		as->rss_pages++;

out:
	spin_unlock_irqrestore(&as->lock, lf);
	return rc;
}

/* ------------------------------------------------- user/kernel boundary */

static bool user_range_ok(const void *p, size_t n)
{
	uintptr_t a = (uintptr_t)p;
	/* Reject the kernel half, and reject wrap-around, which is the trick that
	 * turns a length check into no check at all. */
	if (a + n < a)
		return false;
	return a >= USER_MIN && a + n <= USER_MAX;
}

int copy_from_user(void *dst, const void *usrc, size_t n)
{
	if (!user_range_ok(usrc, n))
		return RK_EFAULT;
	struct task *t = task_current();
	struct address_space *as = t ? t->as : NULL;
	if (!as)
		return RK_EFAULT;

	/* Fault the range in first so the copy itself cannot fail halfway and
	 * leave the destination half written. */
	for (uintptr_t p = ALIGN_DOWN((uintptr_t)usrc, RK_PAGE_SIZE);
	     p < (uintptr_t)usrc + n; p += RK_PAGE_SIZE) {
		paddr_t pa;
		if (!arch_translate(as->pgtable, p, &pa, NULL) &&
		    vm_handle_fault(as, p, VM_FAULT_READ | VM_FAULT_USER) != RK_OK)
			return RK_EFAULT;
	}
	memcpy(dst, usrc, n);
	return RK_OK;
}

int copy_to_user(void *udst, const void *src, size_t n)
{
	if (!user_range_ok(udst, n))
		return RK_EFAULT;
	struct task *t = task_current();
	struct address_space *as = t ? t->as : NULL;
	if (!as)
		return RK_EFAULT;

	for (uintptr_t p = ALIGN_DOWN((uintptr_t)udst, RK_PAGE_SIZE);
	     p < (uintptr_t)udst + n; p += RK_PAGE_SIZE) {
		paddr_t pa;
		u32 prot = 0;
		if (!arch_translate(as->pgtable, p, &pa, &prot)) {
			if (vm_handle_fault(as, p, VM_FAULT_WRITE | VM_FAULT_USER) != RK_OK)
				return RK_EFAULT;
		} else if (!(prot & RK_PROT_WRITE)) {
			if (vm_handle_fault(as, p, VM_FAULT_WRITE | VM_FAULT_USER) != RK_OK)
				return RK_EACCES;
		}
	}
	memcpy(udst, src, n);
	return RK_OK;
}

int strncpy_from_user(char *dst, const char *usrc, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char c;
		if (copy_from_user(&c, usrc + i, 1) != RK_OK)
			return RK_EFAULT;
		dst[i] = c;
		if (!c)
			return (int)i;
	}
	if (n)
		dst[n - 1] = '\0';
	return RK_E2BIG;
}

/* ------------------------------------------------------------------ init */

/* pmm_init runs earlier, from rk_main, because the architecture layer needs a
 * page allocator to finish building page tables before anything else works. */
void mm_init(struct boot_info *bi)
{
	(void)bi;
	kheap_init();

	as_cache   = kmem_cache_create("address_space", sizeof(struct address_space), 16);
	area_cache = kmem_cache_create("vm_area", sizeof(struct vm_area), 16);
	obj_cache  = kmem_cache_create("vm_object", sizeof(struct vm_object), 16);

	memset(&kernel_as, 0, sizeof(kernel_as));
	list_init(&kernel_as.areas);
	spin_lock_init(&kernel_as.lock, "kernel-as");
	kernel_as.pgtable  = arch_pgtable_kernel();
	kernel_as.refcount = 1;

	struct pmm_stats ps;
	pmm_stats(&ps);
	pr_info("virtual memory ready, %pB usable", RK_BYTES(ps.free_pages << RK_PAGE_SHIFT));
}
