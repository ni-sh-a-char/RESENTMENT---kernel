/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - x86_64 page tables.
 *
 * Four levels, 4 KiB pages, with 2 MiB and 1 GiB pages used opportunistically
 * for large aligned ranges because a direct map built out of 4 KiB entries
 * costs both memory and TLB pressure for nothing.
 *
 * A pgtable_t is simply the virtual address of a PML4. Every table is reached
 * through the direct map, which is why the boot stub had to establish that
 * mapping before any of this could run.
 */
#include <arch/x86.h>
#include <rk/arch.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/panic.h>
#include <rk/boot.h>
#include <rk/errno.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "paging"

#define PTE_PRESENT  (1ull << 0)
#define PTE_WRITE    (1ull << 1)
#define PTE_USER     (1ull << 2)
#define PTE_PWT      (1ull << 3)
#define PTE_PCD      (1ull << 4)
#define PTE_ACCESSED (1ull << 5)
#define PTE_DIRTY    (1ull << 6)
#define PTE_HUGE     (1ull << 7)
#define PTE_GLOBAL   (1ull << 8)
#define PTE_COW      (1ull << 9)    /* software bit: copy on write */
#define PTE_SHARED   (1ull << 10)   /* software bit */
#define PTE_NX       (1ull << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

static paddr_t kernel_pml4_phys;
static u64    *kernel_pml4;

vaddr_t arch_phys_to_virt(paddr_t pa) { return (vaddr_t)(DIRECT_MAP_BASE + pa); }

paddr_t arch_virt_to_phys(vaddr_t va)
{
	if (va >= KERNEL_VMA)
		return (paddr_t)(va - KERNEL_VMA);
	if (va >= DIRECT_MAP_BASE)
		return (paddr_t)(va - DIRECT_MAP_BASE);
	/* A user address needs a real walk; callers that hit this are asking the
	 * wrong question and should use arch_translate. */
	return 0;
}

static u64 prot_to_pte(u32 prot)
{
	u64 f = PTE_PRESENT;
	if (prot & RK_PROT_WRITE)   f |= PTE_WRITE;
	if (prot & RK_PROT_USER)    f |= PTE_USER;
	if (prot & RK_PROT_NOCACHE) f |= PTE_PCD | PTE_PWT;
	if (prot & RK_PROT_WC)      f |= PTE_PWT;
	if (prot & RK_PROT_GLOBAL)  f |= PTE_GLOBAL;
	if (!(prot & RK_PROT_EXEC) && (arch_cpu_features() & RK_FEAT_NX))
		f |= PTE_NX;
	return f;
}

static u32 pte_to_prot(u64 e)
{
	u32 p = RK_PROT_READ;
	if (e & PTE_WRITE) p |= RK_PROT_WRITE;
	if (e & PTE_USER)  p |= RK_PROT_USER;
	if (!(e & PTE_NX)) p |= RK_PROT_EXEC;
	if (e & PTE_PCD)   p |= RK_PROT_NOCACHE;
	return p;
}

static u64 *table_at(u64 entry)
{
	return (u64 *)arch_phys_to_virt(entry & PTE_ADDR_MASK);
}

/* Replace a huge entry with a full table of smaller ones covering exactly the
 * same memory with exactly the same permissions.
 *
 * Without this, protecting a single page inside a 2 MiB mapping would either
 * fail or silently change the permissions of everything else in that 2 MiB -
 * which is how hardening the kernel's text ends up making its .bss read-only
 * and the machine faults on the next write. */
static bool split_huge(u64 *table, size_t index, unsigned level)
{
	u64 e = table[index];
	if (!(e & PTE_HUGE))
		return true;

	paddr_t pa = pmm_alloc_page();
	if (!pa)
		return false;

	u64 *child = (u64 *)arch_phys_to_virt(pa);
	u64  flags = e & ~PTE_ADDR_MASK & ~PTE_HUGE;
	/* A 1 GiB entry splits into 2 MiB entries, which stay huge; a 2 MiB entry
	 * splits into ordinary 4 KiB pages. */
	u64  step  = (level == 2) ? (1ull << 21) : RK_PAGE_SIZE;
	u64  base  = e & PTE_ADDR_MASK & ~(step * 512 - 1);
	if (level == 2)
		flags |= PTE_HUGE;

	for (size_t i = 0; i < 512; i++)
		child[i] = (base + i * step) | flags;

	table[index] = pa | PTE_PRESENT | PTE_WRITE | PTE_USER;
	return true;
}

/* Walk one level, optionally creating or splitting the next table.
 * Intermediate entries are always permissive; the leaf carries the real
 * permissions, which is how the hardware combines them anyway. */
static u64 *walk(u64 *table, size_t index, bool create, unsigned level)
{
	u64 e = table[index];

	if (!(e & PTE_PRESENT)) {
		if (!create)
			return NULL;
		paddr_t pa = pmm_alloc_page();
		if (!pa)
			return NULL;
		memset((void *)arch_phys_to_virt(pa), 0, RK_PAGE_SIZE);
		table[index] = pa | PTE_PRESENT | PTE_WRITE | PTE_USER;
		return (u64 *)arch_phys_to_virt(pa);
	}

	if (e & PTE_HUGE) {
		if (!create || !split_huge(table, index, level))
			return NULL;
		e = table[index];
	}
	return table_at(e);
}

static int map_one(u64 *pml4, vaddr_t va, paddr_t pa, u64 flags, unsigned level)
{
	u64 *pdpt = walk(pml4, PML4_IDX(va), true, 3);
	if (!pdpt)
		return RK_ENOMEM;

	if (level == 2) {   /* 1 GiB page */
		pdpt[PDPT_IDX(va)] = (pa & ~0x3FFFFFFFull) | flags | PTE_HUGE;
		return RK_OK;
	}

	u64 *pd = walk(pdpt, PDPT_IDX(va), true, 2);
	if (!pd)
		return RK_ENOMEM;

	if (level == 1) {   /* 2 MiB page */
		pd[PD_IDX(va)] = (pa & ~0x1FFFFFull) | flags | PTE_HUGE;
		return RK_OK;
	}

	u64 *pt = walk(pd, PD_IDX(va), true, 1);
	if (!pt)
		return RK_ENOMEM;

	pt[PT_IDX(va)] = (pa & PTE_ADDR_MASK) | flags;
	return RK_OK;
}

int arch_map(pgtable_t pt, vaddr_t va, paddr_t pa, size_t len, u32 prot)
{
	u64 *pml4 = pt ? (u64 *)pt : kernel_pml4;
	u64  flags = prot_to_pte(prot);
	bool gib = (arch_cpu_features() & RK_FEAT_1GPAGE) != 0;

	if (!IS_ALIGNED(va, RK_PAGE_SIZE) || !IS_ALIGNED(pa, RK_PAGE_SIZE))
		return RK_EINVAL;

	size_t done = 0;
	while (done < len) {
		size_t left = len - done;
		vaddr_t v = va + done;
		paddr_t p = pa + done;
		int r;

		if (gib && left >= (1ul << 30) && IS_ALIGNED(v, 1ul << 30) && IS_ALIGNED(p, 1ul << 30)) {
			r = map_one(pml4, v, p, flags, 2);
			done += 1ul << 30;
		} else if (left >= (1ul << 21) && IS_ALIGNED(v, 1ul << 21) && IS_ALIGNED(p, 1ul << 21)) {
			r = map_one(pml4, v, p, flags, 1);
			done += 1ul << 21;
		} else {
			r = map_one(pml4, v, p, flags, 0);
			done += RK_PAGE_SIZE;
		}
		if (r != RK_OK)
			return r;
	}
	return RK_OK;
}

/* Find the entry that maps va. With split=false it reports whatever granule it
 * finds; with split=true it breaks huge entries apart until it reaches a 4 KiB
 * one, which is what a sub-huge-page permission change needs. */
static u64 *find_leaf_ex(u64 *pml4, vaddr_t va, size_t *page_size, bool split)
{
	u64 *pdpt = walk(pml4, PML4_IDX(va), false, 3);
	if (!pdpt)
		return NULL;

	if ((pdpt[PDPT_IDX(va)] & PTE_HUGE) && !split) {
		*page_size = 1ul << 30;
		return &pdpt[PDPT_IDX(va)];
	}
	u64 *pd = walk(pdpt, PDPT_IDX(va), split, 2);
	if (!pd)
		return NULL;

	if ((pd[PD_IDX(va)] & PTE_HUGE) && !split) {
		*page_size = 1ul << 21;
		return &pd[PD_IDX(va)];
	}
	u64 *ptt = walk(pd, PD_IDX(va), split, 1);
	if (!ptt)
		return NULL;

	*page_size = RK_PAGE_SIZE;
	return &ptt[PT_IDX(va)];
}

static u64 *find_leaf(u64 *pml4, vaddr_t va, size_t *page_size)
{
	return find_leaf_ex(pml4, va, page_size, false);
}

int arch_unmap(pgtable_t pt, vaddr_t va, size_t len)
{
	u64 *pml4 = pt ? (u64 *)pt : kernel_pml4;
	size_t done = 0;

	while (done < len) {
		size_t psize = RK_PAGE_SIZE;
		u64 *leaf = find_leaf(pml4, va + done, &psize);
		if (leaf) {
			*leaf = 0;
			arch_tlb_flush_page(va + done);
		}
		done += psize;
	}
	return RK_OK;
}

int arch_protect(pgtable_t pt, vaddr_t va, size_t len, u32 prot)
{
	u64 *pml4 = pt ? (u64 *)pt : kernel_pml4;
	u64  flags = prot_to_pte(prot);
	size_t done = 0;

	while (done < len) {
		vaddr_t cur = va + done;
		size_t psize = RK_PAGE_SIZE;
		u64 *leaf = find_leaf(pml4, cur, &psize);
		if (!leaf) {
			done += RK_PAGE_SIZE;
			continue;
		}

		/* A huge entry may only be changed wholesale if the request covers
		 * all of it and starts on its boundary. Otherwise split it, or the
		 * change silently applies to megabytes the caller never named. */
		if ((*leaf & PTE_HUGE) &&
		    (!IS_ALIGNED(cur, psize) || len - done < psize)) {
			leaf = find_leaf_ex(pml4, cur, &psize, true);
			if (!leaf) {
				done += RK_PAGE_SIZE;
				continue;
			}
		}

		if (*leaf & PTE_PRESENT) {
			u64 huge = *leaf & PTE_HUGE;
			*leaf = (*leaf & PTE_ADDR_MASK) | flags | huge;
			arch_tlb_flush_page(cur);
		}
		done += psize;
	}
	return RK_OK;
}

bool arch_translate(pgtable_t pt, vaddr_t va, paddr_t *out_pa, u32 *out_prot)
{
	u64 *pml4 = pt ? (u64 *)pt : kernel_pml4;
	size_t psize = RK_PAGE_SIZE;
	u64 *leaf = find_leaf(pml4, va, &psize);

	if (!leaf || !(*leaf & PTE_PRESENT))
		return false;
	if (out_pa)
		*out_pa = (*leaf & PTE_ADDR_MASK) + (va & (psize - 1));
	if (out_prot)
		*out_prot = pte_to_prot(*leaf);
	return true;
}

void arch_tlb_flush_page(vaddr_t va)
{
	__asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
}

void arch_tlb_flush_all(void)
{
	/* Toggling CR4.PGE is the only way to evict global entries. */
	u64 cr4 = read_cr4();
	if (cr4 & (1ull << 7)) {
		write_cr4(cr4 & ~(1ull << 7));
		write_cr4(cr4);
	} else {
		write_cr3(read_cr3());
	}
}

pgtable_t arch_pgtable_kernel(void) { return (pgtable_t)kernel_pml4; }

pgtable_t arch_pgtable_create(void)
{
	paddr_t pa = pmm_alloc_page();
	if (!pa)
		return NULL;
	u64 *t = (u64 *)arch_phys_to_virt(pa);
	memset(t, 0, RK_PAGE_SIZE);

	/* Share the entire upper half with the kernel. One consequence worth
	 * being explicit about: a kernel mapping created later is visible in
	 * every address space without a shootdown, because they all point at the
	 * same PDPTs. */
	for (size_t i = 256; i < 512; i++)
		t[i] = kernel_pml4[i];
	/* Except the recursive slot, which must point at this table. */
	t[RECURSIVE_SLOT] = pa | PTE_PRESENT | PTE_WRITE;
	return (pgtable_t)t;
}

static void free_level(u64 *table, int level)
{
	/* Only the lower half is per-address-space; the upper half is shared with
	 * the kernel and must never be freed here. */
	size_t limit = (level == 3) ? 256 : 512;
	for (size_t i = 0; i < limit; i++) {
		u64 e = table[i];
		if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
			continue;
		if (level > 0)
			free_level(table_at(e), level - 1);
		pmm_free_page(e & PTE_ADDR_MASK);
	}
}

void arch_pgtable_destroy(pgtable_t pt)
{
	if (!pt || (u64 *)pt == kernel_pml4)
		return;
	u64 *t = (u64 *)pt;
	free_level(t, 3);
	pmm_free_page(arch_virt_to_phys((vaddr_t)t));
}

void arch_pgtable_switch(pgtable_t pt)
{
	paddr_t pa = pt ? arch_virt_to_phys((vaddr_t)pt) : kernel_pml4_phys;
	if (pa && pa != (read_cr3() & PTE_ADDR_MASK))
		write_cr3(pa);
}

/* --------------------------------------------------------------- bring-up */

extern char __kernel_phys_start[], __kernel_phys_end[];
extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];

void x86_paging_init(struct boot_info *bi)
{
	kernel_pml4_phys = read_cr3() & PTE_ADDR_MASK;
	kernel_pml4      = (u64 *)arch_phys_to_virt(kernel_pml4_phys);

	/* Extend the direct map past the 4 GiB the boot stub built, if the
	 * machine has more RAM than that. */
	paddr_t highest = 0;
	for (u32 i = 0; i < bi->mmap_count; i++) {
		paddr_t top = bi->mmap[i].base + bi->mmap[i].len;
		if (bi->mmap[i].type == RK_MEM_USABLE && top > highest)
			highest = top;
	}
	if (highest > DIRECT_MAP_SIZE) {
		size_t extra = (size_t)ALIGN_UP(highest - DIRECT_MAP_SIZE, 1ul << 21);
		int r = arch_map(NULL, (vaddr_t)(DIRECT_MAP_BASE + DIRECT_MAP_SIZE),
		                 (paddr_t)DIRECT_MAP_SIZE, extra,
		                 RK_PROT_READ | RK_PROT_WRITE | RK_PROT_GLOBAL);
		if (r == RK_OK)
			pr_info("direct map extended to %pB", RK_BYTES(highest));
		else
			pr_warn("could not extend direct map past 4 GiB: %s", rk_strerror(r));
	}

	/* Now that the kernel runs from the high alias, the identity map is a
	 * liability: it makes every physical page writable through a null-adjacent
	 * address and lets a stray low pointer succeed instead of faulting. */
	kernel_pml4[0] = 0;
	arch_tlb_flush_all();

	pr_info("paging: kernel pml4 at %#llx, identity map removed",
	        (unsigned long long)kernel_pml4_phys);
}

/* Application processors begin in real mode at a low physical address and have
 * to turn paging on while still executing there, so for the duration of start-up
 * the low 2 MiB must be identity mapped. It is put back exactly as it was found
 * afterwards - a permanent low mapping would undo the reason the identity map
 * was dropped in the first place.
 *
 * Safe to remove as soon as every core has reported in, because by then each one
 * is executing from the high alias and never touches a low address again. */
void x86_paging_identity_low(bool on)
{
	if (on) {
		int r = arch_map(NULL, 0, 0, 2ul << 20,
		                 RK_PROT_READ | RK_PROT_WRITE | RK_PROT_EXEC);
		if (r != RK_OK)
			pr_warn("could not identity map low memory for SMP: %s",
			        rk_strerror(r));
	} else {
		arch_unmap(NULL, 0, 2ul << 20);
		kernel_pml4[0] = 0;
	}
	arch_tlb_flush_all();
}

/* Harden the kernel image itself once the memory map is known: text execute
 * only, rodata read only. Done late because the boot stub had to be able to
 * write the pages it was building. */
void x86_paging_harden(void)
{
	size_t text_len   = (size_t)(__text_end - __text_start);
	size_t rodata_len = (size_t)(__rodata_end - __rodata_start);

	arch_protect(NULL, (vaddr_t)(uintptr_t)__text_start, ALIGN_UP(text_len, RK_PAGE_SIZE),
	             RK_PROT_READ | RK_PROT_EXEC | RK_PROT_GLOBAL);
	arch_protect(NULL, (vaddr_t)(uintptr_t)__rodata_start, ALIGN_UP(rodata_len, RK_PAGE_SIZE),
	             RK_PROT_READ | RK_PROT_GLOBAL);
	arch_tlb_flush_all();
	pr_info("kernel image hardened: %pB text r-x, %pB rodata r--",
	        RK_BYTES(text_len), RK_BYTES(rodata_len));
}

/* x86 keeps the instruction and data caches coherent in hardware: self
 * modifying code needs at most a serialising instruction, and the ring
 * transition that follows a program load is one. Nothing to do. */
void arch_sync_icache(vaddr_t va, size_t len)
{
	(void)va;
	(void)len;
}
