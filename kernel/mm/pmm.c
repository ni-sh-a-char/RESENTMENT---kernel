/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - physical page allocator.
 *
 * A buddy allocator, for one reason that matters more than elegance: DMA and
 * accelerators need physically contiguous memory, and a bitmap allocator makes
 * finding a contiguous 2 MiB run an O(n) scan that gets slower as the machine
 * fills up. A buddy makes it O(log n) and makes coalescing automatic.
 *
 * The page array is allocated out of the first usable region large enough to
 * hold it, before any other allocation exists. That bootstrap is the only part
 * of this file that has to be careful about order.
 */
#include <rk/mm.h>
#include <rk/boot.h>
#include <rk/log.h>
#include <rk/string.h>
#include <rk/panic.h>
#include <rk/errno.h>
#include <rk/spinlock.h>
#include <rk/printf.h>
#include <rk/arch.h>

#undef RK_SUBSYS
#define RK_SUBSYS "pmm"

static struct page   *page_array;
static u64            page_count;       /* frames covered by page_array */
static paddr_t        phys_base;        /* address of page_array[0] */
static paddr_t        phys_limit;
static struct list_head free_list[RK_PMM_MAX_ORDER + 1];
static DEFINE_SPINLOCK(pmm_lock);
static struct pmm_stats stats;

/* The DMA zone: memory below 16 MiB, which is the only range some legacy
 * controllers can address. Kept as a separate reserve so that a machine does
 * not run out of it just because something asked for a lot of normal memory. */
#define DMA_LIMIT (16ull << 20)

/* Frame numbers are relative to the lowest usable address, not to zero.
 *
 * On a PC RAM starts at zero and the distinction does not exist. On every ARM
 * and RISC-V board it does: the QEMU virt machine puts RAM at 0x80000000, so
 * indexing from zero would allocate two gigabytes of metadata to describe a
 * hole that contains no memory at all. */
static inline u64 pfn_of(paddr_t pa)
{
	return pa >= phys_base ? (pa - phys_base) >> RK_PAGE_SHIFT : (u64)-1;
}

struct page *pmm_page_of(paddr_t pa)
{
	u64 pfn = pfn_of(pa);
	return pfn < page_count ? &page_array[pfn] : NULL;
}

paddr_t pmm_page_addr(const struct page *p)
{
	return phys_base + ((paddr_t)(p - page_array) << RK_PAGE_SHIFT);
}

static void freelist_add(struct page *p, unsigned order)
{
	p->order = (u16)order;
	p->flags = RK_PG_FREE | RK_PG_HEAD;
	list_add(&p->link, &free_list[order]);
}

static void freelist_del(struct page *p, unsigned order)
{
	(void)order;
	list_del(&p->link);
	p->flags &= (u32)~(RK_PG_FREE | RK_PG_HEAD);
}

/* Publish a run of free frames as the largest aligned buddy blocks that fit.
 *
 * The order matters: this runs only after every reservation is known, so a
 * block is never created and then broken up again. Creating first and
 * reserving afterwards shatters whatever contained a reserved page down to
 * single frames, and on a machine whose firmware reports one large region -
 * which is every ARM and RISC-V board - that means every block becomes order
 * zero and the first request for four contiguous pages fails with half a
 * gigabyte free. */
static void publish_run(u64 first, u64 last)
{
	u64 pfn = first;

	while (pfn < last) {
		unsigned order = 0;
		while (order < RK_PMM_MAX_ORDER) {
			u64 next = 1ull << (order + 1);
			if ((pfn & (next - 1)) != 0)
				break;
			if (pfn + next > last)
				break;
			order++;
		}
		freelist_add(&page_array[pfn], order);
		stats.free_pages += 1ull << order;
		pfn += 1ull << order;
	}
}

void pmm_reserve(paddr_t base, u64 len)
{
	if (!len)
		return;

	paddr_t start = ALIGN_DOWN(base, RK_PAGE_SIZE);
	paddr_t stop  = ALIGN_UP(base + len, RK_PAGE_SIZE);
	if (stop <= phys_base)
		return;
	if (start < phys_base)
		start = phys_base;

	u64 first = pfn_of(start);
	u64 last  = pfn_of(stop);

	for (u64 pfn = first; pfn < last && pfn < page_count; pfn++) {
		page_array[pfn].flags |= RK_PG_RESERVED;
		page_array[pfn].refcount = 1;
	}
}

void pmm_init(struct boot_info *bi)
{
	spin_lock_init(&pmm_lock, "pmm");
	for (unsigned i = 0; i <= RK_PMM_MAX_ORDER; i++)
		list_init(&free_list[i]);

	/* Only usable memory decides the span the page array has to cover.
	 * Firmware routinely reports reserved and MMIO regions far outside RAM -
	 * a PC has them just below 4 GiB, a server far above, an ARM board below
	 * it - and covering those would cost gigabytes of metadata to describe
	 * memory that can never be allocated. Frames outside the array simply
	 * have no struct page, which pmm_page_of already reports. */
	paddr_t lowest = (paddr_t)-1;
	for (u32 i = 0; i < bi->mmap_count; i++) {
		if (bi->mmap[i].type != RK_MEM_USABLE || !bi->mmap[i].len)
			continue;
		paddr_t top = bi->mmap[i].base + bi->mmap[i].len;
		if (bi->mmap[i].base < lowest)
			lowest = bi->mmap[i].base;
		if (top > phys_limit)
			phys_limit = top;
	}
	if (!phys_limit || lowest == (paddr_t)-1)
		panic("pmm: the firmware reported no usable memory at all");

	phys_base  = ALIGN_DOWN(lowest, RK_PAGE_SIZE);
	phys_limit = ALIGN_UP(phys_limit, RK_PAGE_SIZE);
	page_count = (phys_limit - phys_base) >> RK_PAGE_SHIFT;

	size_t array_bytes = (size_t)ALIGN_UP(page_count * sizeof(struct page), RK_PAGE_SIZE);

	/* Place the page array in the first usable region that can hold it and
	 * that sits above the kernel image, so we never overwrite ourselves. */
	/* Two passes. The first keeps the metadata out of the DMA zone, because
	 * that is the only memory some controllers can reach and spending it on
	 * bookkeeping would be a poor trade. The second drops that preference
	 * rather than failing to boot on a machine that has nothing else. */
	paddr_t array_phys = 0;
	paddr_t kernel_top = ALIGN_UP(bi->kernel_phys_end, RK_PAGE_SIZE);

	for (int pass = 0; pass < 2 && !array_phys; pass++) {
		for (u32 i = 0; i < bi->mmap_count; i++) {
			if (bi->mmap[i].type != RK_MEM_USABLE)
				continue;
			paddr_t base = bi->mmap[i].base;
			paddr_t end  = base + bi->mmap[i].len;

			if (base < kernel_top && kernel_top < end)
				base = kernel_top;
			if (pass == 0 && base < DMA_LIMIT)
				base = DMA_LIMIT;
			base = ALIGN_UP(base, RK_PAGE_SIZE);

			if (end > base && end - base >= array_bytes) {
				array_phys = base;
				break;
			}
		}
	}
	if (!array_phys)
		panic("pmm: no region large enough for %pB of page metadata", RK_BYTES(array_bytes));

	page_array = (struct page *)arch_phys_to_virt(array_phys);
	memset(page_array, 0, array_bytes);

	/* Everything starts reserved. Frames become available only by appearing
	 * in a usable region and surviving every reservation, which is the safe
	 * direction to be wrong in. */
	for (u64 i = 0; i < page_count; i++) {
		list_init(&page_array[i].link);
		page_array[i].flags = RK_PG_RESERVED;
		page_array[i].refcount = 1;
	}

	for (u32 i = 0; i < bi->mmap_count; i++) {
		if (bi->mmap[i].type != RK_MEM_USABLE)
			continue;
		paddr_t base = bi->mmap[i].base;
		paddr_t end  = base + bi->mmap[i].len;

		/* The first page is never handed out: a null pointer must fault. */
		if (base < phys_base + RK_PAGE_SIZE)
			base = phys_base + RK_PAGE_SIZE;
		if (end <= base)
			continue;

		u64 first = pfn_of(ALIGN_UP(base, RK_PAGE_SIZE));
		u64 last  = pfn_of(ALIGN_DOWN(end, RK_PAGE_SIZE));
		for (u64 pfn = first; pfn < last && pfn < page_count; pfn++) {
			page_array[pfn].flags = 0;
			page_array[pfn].refcount = 0;
		}
	}

	/* Everything that is in use, before a single block is published. */

	/* The first megabyte is never allocatable. Firmware tables, the real-mode
	 * interrupt vectors and the BIOS data area live there, and the application
	 * processor trampoline has to be copied to a fixed low page because a
	 * startup interrupt carries nothing but a page number. On a machine whose
	 * RAM starts higher this reserve falls outside the page array entirely and
	 * costs nothing. */
	pmm_reserve(0, 0x100000);

	pmm_reserve(bi->kernel_phys_start, bi->kernel_phys_end - bi->kernel_phys_start);
	pmm_reserve(array_phys, array_bytes);
	if (bi->initrd_start && bi->initrd_end > bi->initrd_start)
		pmm_reserve(bi->initrd_start, bi->initrd_end - bi->initrd_start);
	for (u32 i = 0; i < bi->module_count; i++)
		pmm_reserve(bi->modules[i].start, bi->modules[i].end - bi->modules[i].start);
	for (u32 i = 0; i < bi->mmap_count; i++)
		if (bi->mmap[i].type != RK_MEM_USABLE)
			pmm_reserve(bi->mmap[i].base, bi->mmap[i].len);

	/* Now publish, in maximal aligned blocks. Each free run is walked once. */
	u64 run_start = 0;
	bool in_run = false;
	for (u64 pfn = 0; pfn <= page_count; pfn++) {
		bool usable = pfn < page_count &&
		              !(page_array[pfn].flags & RK_PG_RESERVED);
		if (usable && !in_run) {
			run_start = pfn;
			in_run = true;
		} else if (!usable && in_run) {
			publish_run(run_start, pfn);
			in_run = false;
		}
	}

	stats.total_pages = page_count;
	stats.reserved_pages = page_count - stats.free_pages;

	struct pmm_stats probe;
	pmm_stats(&probe);

	pr_info("physical memory: %pB usable of %pB mapped, %llu frames from %#llx",
	        RK_BYTES(stats.free_pages << RK_PAGE_SHIFT),
	        RK_BYTES(page_count << RK_PAGE_SHIFT),
	        (unsigned long long)page_count,
	        (unsigned long long)phys_base);
	pr_info("page metadata: %pB at %#llx, largest free block %pB",
	        RK_BYTES(array_bytes), (unsigned long long)array_phys,
	        RK_BYTES((u64)RK_PAGE_SIZE << probe.largest_free_order));
}

/* ------------------------------------------------------------ allocation */

static struct page *buddy_alloc(unsigned order, u32 flags)
{
	for (unsigned o = order; o <= RK_PMM_MAX_ORDER; o++) {
		struct page *p = NULL;

		if (flags & RK_PG_DMA) {
			/* Scan for a block that is actually below the DMA limit rather
			 * than taking the head and hoping. */
			struct page *cand;
			list_for_each_entry(cand, &free_list[o], link) {
				if (pmm_page_addr(cand) + ((u64)RK_PAGE_SIZE << o) <= DMA_LIMIT) {
					p = cand;
					break;
				}
			}
			if (!p)
				continue;
		} else {
			if (list_empty(&free_list[o]))
				continue;
			p = list_first_entry(&free_list[o], struct page, link);
		}

		freelist_del(p, o);

		/* Split down to the requested order, returning the halves. */
		u64 pfn = (u64)(p - page_array);
		for (unsigned k = o; k > order; k--) {
			u64 buddy = pfn + (1ull << (k - 1));
			freelist_add(&page_array[buddy], k - 1);
		}

		p->order    = (u16)order;
		p->refcount = 1;
		p->flags    = 0;
		stats.free_pages -= 1ull << order;
		stats.used_pages += 1ull << order;
		stats.alloc_count++;
		return p;
	}
	stats.fail_count++;
	return NULL;
}

paddr_t pmm_alloc_pages(unsigned order, u32 flags)
{
	if (order > RK_PMM_MAX_ORDER)
		return 0;

	unsigned long f = spin_lock_irqsave(&pmm_lock);
	struct page *p = buddy_alloc(order, flags);
	spin_unlock_irqrestore(&pmm_lock, f);

	if (!p)
		return 0;
	paddr_t pa = pmm_page_addr(p);
	if (!(flags & RK_PG_LOCKED))
		memset((void *)arch_phys_to_virt(pa), 0, (size_t)RK_PAGE_SIZE << order);
	return pa;
}

paddr_t pmm_alloc_page(void) { return pmm_alloc_pages(0, 0); }

static void buddy_free(u64 pfn, unsigned order)
{
	while (order < RK_PMM_MAX_ORDER) {
		u64 buddy = pfn ^ (1ull << order);
		if (buddy >= page_count)
			break;
		struct page *b = &page_array[buddy];
		/* Only merge with a free buddy of exactly this order. */
		if (!(b->flags & RK_PG_FREE) || !(b->flags & RK_PG_HEAD) || b->order != order)
			break;
		freelist_del(b, order);
		pfn = pfn < buddy ? pfn : buddy;
		order++;
	}
	freelist_add(&page_array[pfn], order);
}

void pmm_free_pages(paddr_t pa, unsigned order)
{
	u64 pfn = pfn_of(pa);
	if (pfn >= page_count || order > RK_PMM_MAX_ORDER)
		return;

	struct page *p = &page_array[pfn];
	if (p->flags & RK_PG_FREE) {
		pr_err("double free of physical page %#llx", (unsigned long long)pa);
		return;
	}
	if (p->flags & RK_PG_RESERVED)
		return;

	unsigned long f = spin_lock_irqsave(&pmm_lock);
	p->refcount = 0;
	p->owner = NULL;
	buddy_free(pfn, order);
	stats.free_pages += 1ull << order;
	stats.used_pages -= 1ull << order;
	stats.free_count++;
	spin_unlock_irqrestore(&pmm_lock, f);
}

void pmm_free_page(paddr_t pa) { pmm_free_pages(pa, 0); }

void pmm_page_get(paddr_t pa)
{
	struct page *p = pmm_page_of(pa);
	if (!p)
		return;
	unsigned long f = spin_lock_irqsave(&pmm_lock);
	p->refcount++;
	spin_unlock_irqrestore(&pmm_lock, f);
}

bool pmm_page_put(paddr_t pa)
{
	struct page *p = pmm_page_of(pa);
	if (!p)
		return false;

	unsigned long f = spin_lock_irqsave(&pmm_lock);
	bool last = (p->refcount <= 1);
	if (!last)
		p->refcount--;
	spin_unlock_irqrestore(&pmm_lock, f);

	if (last)
		pmm_free_page(pa);
	return last;
}

void pmm_stats(struct pmm_stats *out)
{
	unsigned long f = spin_lock_irqsave(&pmm_lock);
	*out = stats;
	out->largest_free_order = 0;
	for (unsigned o = RK_PMM_MAX_ORDER + 1; o-- > 0;) {
		if (!list_empty(&free_list[o])) {
			out->largest_free_order = o;
			break;
		}
	}
	spin_unlock_irqrestore(&pmm_lock, f);
}
