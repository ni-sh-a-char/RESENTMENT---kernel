/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - memory management.
 *
 * Three layers, deliberately small:
 *   pmm    buddy allocator over physical page frames
 *   kheap  slab caches for fixed-size objects, plus a general kmalloc
 *   vmm    address spaces built from vm_objects mapped into vm_areas
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/arch.h>
#include <rk/spinlock.h>

/* ------------------------------------------------------ physical allocator */

#define RK_PMM_MAX_ORDER 18   /* 2^18 pages = 1 GiB largest contiguous block */

/* One per physical frame. Kept small: on a 64 GiB machine there are 16M of
 * these, so every extra 8 bytes costs 128 MiB of RAM. */
struct page {
	struct list_head link;
	u32   flags;
	u16   order;      /* valid on a buddy head */
	u16   refcount;
	void *owner;      /* vm_object or slab cache, for diagnostics */
};

#define RK_PG_FREE     (1u << 0)
#define RK_PG_HEAD     (1u << 1)
#define RK_PG_SLAB     (1u << 2)
#define RK_PG_RESERVED (1u << 3)
#define RK_PG_DMA      (1u << 4)   /* below 16 MiB, for legacy ISA DMA */
#define RK_PG_LOCKED   (1u << 5)
#define RK_PG_DIRTY    (1u << 6)

void    pmm_init(struct boot_info *bi);
paddr_t pmm_alloc_pages(unsigned order, u32 flags);
paddr_t pmm_alloc_page(void);
void    pmm_free_pages(paddr_t pa, unsigned order);
void    pmm_free_page(paddr_t pa);
void    pmm_reserve(paddr_t base, u64 len);

struct page *pmm_page_of(paddr_t pa);
paddr_t      pmm_page_addr(const struct page *p);
void         pmm_page_get(paddr_t pa);
bool         pmm_page_put(paddr_t pa);   /* true if it hit zero and was freed */

struct pmm_stats {
	u64 total_pages, free_pages, reserved_pages, used_pages;
	u64 alloc_count, free_count, fail_count;
	u64 largest_free_order;
};
void pmm_stats(struct pmm_stats *out);

/* ---------------------------------------------------------- kernel heap */

struct kmem_cache;

struct kmem_cache *kmem_cache_create(const char *name, size_t objsize, size_t align);
void  *kmem_cache_alloc(struct kmem_cache *c);
void   kmem_cache_free(struct kmem_cache *c, void *obj);
void   kmem_cache_destroy(struct kmem_cache *c);

void   kheap_init(void);
void  *kmalloc(size_t n);
void  *kmalloc_aligned(size_t n, size_t align);
void  *kzalloc(size_t n);
void  *kcalloc(size_t count, size_t size);
void  *krealloc(void *p, size_t n);
void   kfree(void *p);
size_t ksize(const void *p);
char  *kstrdup(const char *s);
void  *kmemdup(const void *p, size_t n);

struct kheap_stats {
	u64 bytes_live, bytes_peak, allocs, frees;
	u64 slab_pages, large_pages;
};
void kheap_stats(struct kheap_stats *out);

/* ---------------------------------------------------------- virtual memory */

/* A vm_object is a page-granular store: anonymous memory, a file mapping, or
 * device MMIO. Address spaces map slices of them, which is what makes fork,
 * shared memory and zero-copy IPC all the same mechanism. */
enum vm_object_kind {
	VM_OBJ_ANON = 0,
	VM_OBJ_PHYS,      /* fixed physical range, e.g. a framebuffer */
	VM_OBJ_FILE,
	VM_OBJ_SHARED,
};

struct vm_object {
	enum vm_object_kind kind;
	size_t   npages;
	paddr_t *frames;      /* lazily populated; 0 means not resident */
	paddr_t  phys_base;   /* VM_OBJ_PHYS only */
	void    *backing;     /* vfs node for VM_OBJ_FILE */
	u32      refcount;
	spinlock_t lock;
	const char *name;
};

struct vm_object *vm_object_anon(size_t npages, const char *name);
struct vm_object *vm_object_phys(paddr_t base, size_t npages, const char *name);
void vm_object_get(struct vm_object *o);
void vm_object_put(struct vm_object *o);

struct vm_area {
	struct list_head  link;
	vaddr_t           start, end;
	u32               prot;
	u32               flags;
	struct vm_object *obj;
	size_t            obj_offset;   /* in pages */
	const char       *name;
};

#define VM_AREA_FIXED   (1u << 0)
#define VM_AREA_GROWS   (1u << 1)   /* stack: fault below start extends it */
#define VM_AREA_SHARED  (1u << 2)
#define VM_AREA_LOCKED  (1u << 3)   /* never evict; required for DMA buffers */

struct address_space {
	pgtable_t        pgtable;
	struct list_head areas;
	vaddr_t          mmap_base;   /* where anonymous mmaps start growing */
	vaddr_t          brk;
	spinlock_t       lock;
	u32              refcount;
	u64              rss_pages;
	rk_id_t          id;
};

struct address_space *as_create(void);
struct address_space *as_kernel(void);
void  as_destroy(struct address_space *as);
void  as_switch(struct address_space *as);
struct address_space *as_clone(struct address_space *src);  /* copy-on-write */

int  as_map_object(struct address_space *as, vaddr_t hint, struct vm_object *obj,
                   size_t obj_offset, size_t npages, u32 prot, u32 flags,
                   const char *name, vaddr_t *out);
int  as_map_anon(struct address_space *as, vaddr_t hint, size_t len, u32 prot,
                 const char *name, vaddr_t *out);
int  as_unmap(struct address_space *as, vaddr_t va, size_t len);
int  as_protect(struct address_space *as, vaddr_t va, size_t len, u32 prot);
struct vm_area *as_find(struct address_space *as, vaddr_t va);

/* Returns RK_OK if the fault was resolved (demand page, COW, stack growth). */
int  vm_handle_fault(struct address_space *as, vaddr_t va, u32 access);

#define VM_FAULT_READ  (1u << 0)
#define VM_FAULT_WRITE (1u << 1)
#define VM_FAULT_EXEC  (1u << 2)
#define VM_FAULT_USER  (1u << 3)

/* Bounded copies across the user/kernel trust boundary. Never dereference a
 * user pointer directly; these validate and fault safely. */
int copy_from_user(void *dst, const void *usrc, size_t n) __must_check;
int copy_to_user(void *udst, const void *src, size_t n) __must_check;
int strncpy_from_user(char *dst, const char *usrc, size_t n) __must_check;

/* Where user programs live, and why it differs per architecture.
 *
 * x86_64 puts the kernel in the top half of a 48-bit space, so the entire
 * lower canonical half belongs to userspace and the layout is the familiar
 * one.
 *
 * aarch64 here is identity mapped through TTBR0 with no higher half, so the
 * kernel occupies the bottom eight gigabytes of *every* address space -
 * including a user one, which must contain the kernel's mappings or a system
 * call would unmap the code servicing it. User programs therefore start above
 * that, and the 39-bit space leaves 504 GiB of room there. Moving the kernel
 * to TTBR1 would free the bottom of the space and is the right long-term
 * shape; it is a larger change than it looks, because every kernel pointer in
 * the port is currently a physical address.
 *
 * riscv64 runs with address translation switched off altogether. There is no
 * userspace to place until it has an MMU, and the constants below say so by
 * describing an empty range rather than by pretending. */
#if defined(RK_ARCH_X86_64)
#  define RK_USER_VA_MIN    0x0000000000400000ull
#  define RK_USER_VA_MAX    0x00007FFFFFFFF000ull
#  define RK_USER_STACK_TOP 0x0000700000000000ull
#  define RK_HAVE_USERSPACE 1
#elif defined(RK_ARCH_AARCH64)
#  define RK_USER_VA_MIN    0x0000000200000000ull
#  define RK_USER_VA_MAX    0x0000007FFFFFF000ull
#  define RK_USER_STACK_TOP 0x0000007F00000000ull
#  define RK_HAVE_USERSPACE 1
#elif defined(RK_ARCH_RISCV64)
   /* Sv39, identity mapped through the bottom eight gigabytes exactly as the
    * aarch64 port is, so userspace lives above that.
    *
    * The ceiling is 256 GiB rather than the 512 GiB the three levels describe,
    * and that is not a rounding-down for comfort. Sv39 requires a canonical
    * address: bits 63:39 must all equal bit 38. An address with bit 38 set and
    * zeros above it is not merely unmapped, it is malformed, and the hardware
    * faults on it however correct the page tables are. So the usable lower
    * half stops just short of 2^38. */
#  define RK_USER_VA_MIN    0x0000000200000000ull
#  define RK_USER_VA_MAX    0x0000003FFFFFF000ull
#  define RK_USER_STACK_TOP 0x0000003F00000000ull
#  define RK_HAVE_USERSPACE 1
#else
#  define RK_USER_VA_MIN    0x0000000000400000ull
#  define RK_USER_VA_MAX    0x0000000000400000ull
#  define RK_USER_STACK_TOP 0x0000000000400000ull
#  define RK_HAVE_USERSPACE 0
#endif

/* Where a kernel mapping with no address hint lands.
 *
 * The kernel address space had no mmap base at all - it is a static that is
 * memset to zero - so every hintless as_map_anon into it resolved to address
 * zero, which as_map_object could not distinguish from failure and reported as
 * out of memory. Nothing in the kernel had asked for one, so it was never
 * noticed.
 *
 * x86_64 has a whole canonical half; this sits above the direct map with room
 * either side. The other two are identity mapped with the kernel in the bottom
 * eight gigabytes, so this goes above that and stays inside the 39-bit space
 * their page tables describe. */
#if defined(RK_ARCH_X86_64)
#  define RK_KERN_MMAP_BASE 0xFFFFC00000000000ull
#else
#  define RK_KERN_MMAP_BASE 0x0000000400000000ull
#endif

/* Whether this architecture translates addresses at all. Every port does now;
 * the switch is kept because a bring-up runs without paging before it runs
 * with it, and the self-test should say "skipped" rather than fail. */
#define RK_HAVE_PAGING 1

void mm_init(struct boot_info *bi);
