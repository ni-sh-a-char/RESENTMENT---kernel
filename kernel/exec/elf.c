/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - loading a program and running it in ring 3.
 *
 * The whole of this file treats the image as hostile input. An ELF header is
 * a structure an attacker fully controls, and the classic loader bugs are all
 * arithmetic: an offset plus a length that wraps, a segment that claims to be
 * larger in memory than in the file and is then used to read past the buffer,
 * a virtual address that lands on kernel memory. Every one of those is checked
 * here, once, before anything is mapped.
 *
 * What this does not do is dynamic linking. A dynamic linker belongs in
 * userspace: putting one here would give the kernel opinions about libraries,
 * search paths and symbol versioning, none of which it has any business
 * holding. Static executables and self-relocating PIEs cover everything this
 * system needs to run.
 */
#include <rk/elf.h>
#include <rk/mm.h>
#include <rk/vfs.h>
#include <rk/sched.h>
#include <rk/arch.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/log.h>
#include <rk/panic.h>
#include <rk/cap.h>
#include <rk/graph.h>
#include <rk/printf.h>
#include <rk/time.h>

#undef RK_SUBSYS
#define RK_SUBSYS "exec"

/* Sixteen megabytes. Large enough for anything this system runs, small enough
 * that a corrupt header cannot ask the allocator for the machine. */
#define ELF_MAX_IMAGE (16ull << 20)
#define ELF_MAX_PHNUM 64

/* ------------------------------------------------------------ validation */

/* Does [off, off+len) lie inside a buffer of `size` bytes, without the
 * addition wrapping? Every bounds check in this file goes through here, so
 * there is one place to get it right rather than nine. */
static bool within(u64 off, u64 len, size_t size)
{
	return off <= size && len <= (u64)size - off;
}

int rk_elf_check(const void *data, size_t len)
{
	if (!data || len < sizeof(struct elf64_ehdr))
		return RK_EINVAL;

	const struct elf64_ehdr *eh = data;

	if (memcmp(eh->e_ident, ELFMAG, 4) != 0)
		return RK_ENOEXEC;
	if (eh->e_ident[EI_CLASS] != ELFCLASS64)
		return RK_ENOEXEC;
	if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
		return RK_ENOEXEC;
	if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
		return RK_ENOEXEC;
	if (eh->e_machine != RK_ELF_MACHINE)
		return RK_ENOEXEC;
	if (eh->e_phentsize != sizeof(struct elf64_phdr))
		return RK_ENOEXEC;
	if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHNUM)
		return RK_ENOEXEC;
	if (!within(eh->e_phoff, (u64)eh->e_phnum * sizeof(struct elf64_phdr), len))
		return RK_ENOEXEC;

	return RK_OK;
}

static u32 prot_of(u32 p_flags)
{
	u32 prot = RK_PROT_USER;
	if (p_flags & PF_R) prot |= RK_PROT_READ;
	if (p_flags & PF_W) prot |= RK_PROT_WRITE;
	if (p_flags & PF_X) prot |= RK_PROT_EXEC;
	/* A segment with no access at all is not useful and is usually a sign of
	 * a header that has been tampered with. Give it read, so that a fault on
	 * it is a fault on the instruction rather than on the mapping. */
	if (!(prot & (RK_PROT_READ | RK_PROT_WRITE | RK_PROT_EXEC)))
		prot |= RK_PROT_READ;
	return prot;
}

/* ---------------------------------------------------------------- loading */

int rk_elf_load(struct address_space *as, const void *data, size_t len,
                struct rk_exec_image *out)
{
	int r = rk_elf_check(data, len);
	if (r != RK_OK)
		return r;

	const struct elf64_ehdr *eh = data;
	const struct elf64_phdr *ph =
		(const struct elf64_phdr *)((const u8 *)data + eh->e_phoff);

	/* A position independent image is placed once, at a fixed bias, and every
	 * segment moves with it. Choosing the bias here rather than per segment is
	 * what keeps the relative layout the linker computed intact. */
	u64 bias = 0;
	if (eh->e_type == ET_DYN)
		bias = 0x0000100000000000ull;

	u64 brk = 0;
	size_t mapped_pages = 0;
	bool any = false;

	/* First pass: validate every header before mapping any of them, so a bad
	 * segment halfway down does not leave a half-built address space. */
	for (u16 i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;

		if (ph[i].p_filesz > ph[i].p_memsz)
			return RK_ENOEXEC;
		if (!within(ph[i].p_offset, ph[i].p_filesz, len))
			return RK_ENOEXEC;
		if (ph[i].p_memsz > ELF_MAX_IMAGE)
			return RK_ENOEXEC;

		u64 start = ph[i].p_vaddr + bias;
		u64 end   = start + ph[i].p_memsz;
		if (end < start)                        /* wrapped */
			return RK_ENOEXEC;
		if (start < RK_USER_LOAD_MIN || end > RK_USER_MAX)
			return RK_ENOEXEC;

		any = true;
	}
	if (!any)
		return RK_ENOEXEC;

	/* Second pass: map, then fill. */
	for (u16 i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;

		u64 start   = ph[i].p_vaddr + bias;
		u64 aligned = ALIGN_DOWN(start, RK_PAGE_SIZE);
		u64 end     = ALIGN_UP(start + ph[i].p_memsz, RK_PAGE_SIZE);
		size_t span = (size_t)(end - aligned);

		/* Mapped writable regardless of what the segment asks for, because the
		 * contents have to be written into it. The real protection is applied
		 * below, once the bytes are in place. */
		vaddr_t at = 0;
		int rc = as_map_anon(as, (vaddr_t)aligned, span,
		                     RK_PROT_READ | RK_PROT_WRITE | RK_PROT_USER,
		                     "elf", &at);
		if (rc != RK_OK)
			return rc;

		/* Anonymous memory arrives zeroed, so .bss needs nothing beyond the
		 * mapping: only the file-backed prefix is copied. */
		if (ph[i].p_filesz) {
			memcpy((void *)(uintptr_t)start,
			       (const u8 *)data + ph[i].p_offset,
			       (size_t)ph[i].p_filesz);		}


		/* The bytes were just written through a data mapping. On an
		 * architecture whose caches are not coherent, the instruction fetcher
		 * has not seen them, and the program faults on its first instruction
		 * with an error that says nothing about caches. */
		if (ph[i].p_flags & PF_X)
			arch_sync_icache((vaddr_t)aligned, span);

		rc = as_protect(as, (vaddr_t)aligned, span, prot_of(ph[i].p_flags));
		if (rc != RK_OK)
			return rc;

		mapped_pages += span / RK_PAGE_SIZE;
		if (end > brk)
			brk = end;
	}

	u64 entry = eh->e_entry + bias;
	if (entry < RK_USER_LOAD_MIN || entry >= RK_USER_MAX)
		return RK_ENOEXEC;

	out->entry     = (vaddr_t)entry;
	out->brk       = (vaddr_t)brk;
	out->load_bias = (vaddr_t)bias;
	out->npages    = mapped_pages;
	out->stack_top = 0;   /* the caller builds the stack */
	return RK_OK;
}

/* ------------------------------------------------------------------ stack */

/* The initial stack. Deliberately minimal and deliberately documented:
 *
 *   stack_top ->  [ argv strings              ]
 *                 [ NULL                      ]  end of argv
 *                 [ argv[argc-1] ... argv[0]  ]
 *   sp        ->  [ argc                      ]
 *
 * No environment and no auxiliary vector, because nothing here consumes them
 * and inventing an ABI nobody uses is how a kernel accumulates obligations it
 * cannot later drop. The shape above is the System V prefix, so adding envp
 * and auxv later is an append rather than a redesign. */
static int build_stack(struct address_space *as, int argc,
                       const char *const argv[], vaddr_t *out_sp)
{
	vaddr_t base = 0;
	int rc = as_map_anon(as, (vaddr_t)(RK_USER_STACK_TOP - RK_USER_STACK_SIZE),
	                     RK_USER_STACK_SIZE,
	                     RK_PROT_READ | RK_PROT_WRITE | RK_PROT_USER,
	                     "stack", &base);
	if (rc != RK_OK)
		return rc;

	u8 *top = (u8 *)(uintptr_t)(base + RK_USER_STACK_SIZE);
	u8 *p   = top;

	if (argc < 0)
		argc = 0;
	if (argc > 32)
		argc = 32;

	u64 slot[32];
	for (int i = argc - 1; i >= 0; i--) {
		size_t n = strlen(argv[i]) + 1;
		/* A pathological argument must not walk the stack mapping off its
		 * bottom and into whatever is below. */
		if ((size_t)(p - (u8 *)(uintptr_t)base) < n + 512)
			return RK_E2BIG;
		p -= n;
		memcpy(p, argv[i], n);
		slot[i] = (u64)(uintptr_t)p;
	}

	/* The entry point needs two things at once: a sixteen-byte aligned stack
	 * pointer, and argc at exactly [sp]. Aligning after the pushes satisfies
	 * the first and breaks the second - it slides the stack pointer down past
	 * argc, and the program reads whatever padding landed there instead.
	 *
	 * So the padding goes in first, sized from what is about to be pushed. */
	u64 sp = ALIGN_DOWN((u64)(uintptr_t)p, 16);
	size_t words = 1 + (size_t)argc + 1;      /* argc, argv[], the NULL */
	if (words & 1)
		sp -= 8;

	sp -= 8;                                  /* NULL terminating argv */
	*(u64 *)(uintptr_t)sp = 0;
	for (int i = argc - 1; i >= 0; i--) {
		sp -= 8;
		*(u64 *)(uintptr_t)sp = slot[i];
	}
	sp -= 8;
	*(u64 *)(uintptr_t)sp = (u64)argc;

	RK_ASSERT_MSG((sp & 15) == 0, "user stack pointer %#llx is not aligned",
	              (unsigned long long)sp);

	*out_sp = (vaddr_t)sp;
	return RK_OK;
}

/* ------------------------------------------------------------- the process */

struct spawn_arg {
	void  *image;
	size_t len;
	char   path[128];
	int    argc;
	char   argv[4][64];
};

/* Runs as the new task's first thread, already in the new address space
 * because the scheduler installed the task's page table on the way in. That
 * is what lets the loader write segment contents through their final user
 * addresses instead of building a second mapping for them. */
static void user_bootstrap(void *arg)
{
	struct spawn_arg *sa = arg;
	struct task *task = task_current();
	struct rk_exec_image img;

	int rc = rk_elf_load(task->as, sa->image, sa->len, &img);
	if (rc != RK_OK) {
		pr_warn("%s: not loadable: %s", sa->path, rk_strerror(rc));
		goto out;
	}

	const char *argv[4];
	for (int i = 0; i < sa->argc && i < 4; i++)
		argv[i] = sa->argv[i];

	vaddr_t sp = 0;
	rc = build_stack(task->as, sa->argc, argv, &sp);
	if (rc != RK_OK) {
		pr_warn("%s: cannot build the initial stack: %s",
		        sa->path, rk_strerror(rc));
		goto out;
	}

	pr_info("%s: entry %#llx, stack %#llx, %llu pages",
	        sa->path, (unsigned long long)img.entry,
	        (unsigned long long)sp, (unsigned long long)img.npages);

	rk_graph_record(GEV_EXEC, task->pid, img.entry, img.npages,
	                rk_time_ns());

	kfree(sa->image);
	sa->image = NULL;
	kfree(sa);

	/* Never returns unless the entry could not be taken at all. */
	rc = arch_enter_user(img.entry, sp, NULL);
	pr_err("%s: could not enter user mode: %s", sa->path, rk_strerror(rc));
	thread_exit(rc);

out:
	if (sa->image)
		kfree(sa->image);
	kfree(sa);
	thread_exit(rc);
}

int rk_exec_spawn(const char *path, int argc, const char *const argv[],
                  struct task **out)
{
	void *data = NULL;
	size_t len = 0;

	int rc = rk_vfs_read_file(path, &data, &len);
	if (rc != RK_OK)
		return rc;
	if (len > ELF_MAX_IMAGE) {
		kfree(data);
		return RK_E2BIG;
	}

	rc = rk_elf_check(data, len);
	if (rc != RK_OK) {
		kfree(data);
		return rc;
	}

	struct spawn_arg *sa = kzalloc(sizeof(*sa));
	if (!sa) {
		kfree(data);
		return RK_ENOMEM;
	}
	sa->image = data;
	sa->len   = len;
	strlcpy(sa->path, path, sizeof(sa->path));
	sa->argc = argc < 0 ? 0 : (argc > 4 ? 4 : argc);
	for (int i = 0; i < sa->argc; i++)
		strlcpy(sa->argv[i], argv[i], sizeof(sa->argv[i]));

	const char *base = path;
	for (const char *s = path; *s; s++)
		if (*s == '/')
			base = s + 1;

	struct task *task = task_create(base, task_current());
	if (!task) {
		kfree(data);
		kfree(sa);
		return RK_ENOMEM;
	}

	/* Its own address space, and therefore its own everything: a task with no
	 * capability in its space can do nothing but compute and exit, which is
	 * exactly what a freshly loaded program should be able to do until
	 * somebody hands it more. */
	task->as = as_create();
	if (!task->as) {
		task_exit(task, RK_ENOMEM);
		kfree(data);
		kfree(sa);
		return RK_ENOMEM;
	}

	struct thread *t = thread_create_in(task, base, user_bootstrap, sa,
	                                    SCHED_INTERACTIVE, RK_PRIO_DEFAULT);
	if (!t) {
		task_exit(task, RK_ENOMEM);
		kfree(data);
		kfree(sa);
		return RK_ENOMEM;
	}

	thread_start(t);

	if (out)
		*out = task;
	return RK_OK;
}
