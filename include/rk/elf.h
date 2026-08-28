/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - ELF64 program images.
 *
 * Only what a loader needs. This kernel loads static ET_EXEC and position
 * independent ET_DYN images; it deliberately does not implement dynamic
 * linking, because a dynamic linker is a userspace concern and putting one in
 * the kernel means the kernel has opinions about libraries.
 *
 * Every field an untrusted file controls is validated before it is used. An
 * ELF header is attacker-supplied data in exactly the same sense a network
 * packet is, and the loader treats it that way.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>
#include <rk/mm.h>

/* e_ident */
#define EI_NIDENT      16
#define ELFMAG         "\177ELF"
#define EI_CLASS       4
#define ELFCLASS64     2
#define EI_DATA        5
#define ELFDATA2LSB    1
#define EI_VERSION     6
#define EV_CURRENT     1

/* e_type */
#define ET_NONE        0
#define ET_REL         1
#define ET_EXEC        2
#define ET_DYN         3

/* e_machine */
#define EM_X86_64      62
#define EM_AARCH64     183
#define EM_RISCV       243

#if defined(RK_ARCH_X86_64)
#  define RK_ELF_MACHINE EM_X86_64
#elif defined(RK_ARCH_AARCH64)
#  define RK_ELF_MACHINE EM_AARCH64
#elif defined(RK_ARCH_RISCV64)
#  define RK_ELF_MACHINE EM_RISCV
#else
#  error "no ELF machine type for this architecture"
#endif

/* p_type */
#define PT_NULL        0
#define PT_LOAD        1
#define PT_DYNAMIC     2
#define PT_INTERP      3
#define PT_NOTE        4
#define PT_PHDR        6
#define PT_TLS         7
#define PT_GNU_STACK   0x6474e551
#define PT_GNU_RELRO   0x6474e552

/* p_flags */
#define PF_X           1
#define PF_W           2
#define PF_R           4

struct elf64_ehdr {
	u8  e_ident[EI_NIDENT];
	u16 e_type;
	u16 e_machine;
	u32 e_version;
	u64 e_entry;
	u64 e_phoff;
	u64 e_shoff;
	u32 e_flags;
	u16 e_ehsize;
	u16 e_phentsize;
	u16 e_phnum;
	u16 e_shentsize;
	u16 e_shnum;
	u16 e_shstrndx;
} __packed;

struct elf64_phdr {
	u32 p_type;
	u32 p_flags;
	u64 p_offset;
	u64 p_vaddr;
	u64 p_paddr;
	u64 p_filesz;
	u64 p_memsz;
	u64 p_align;
} __packed;

/* Where a loaded image put things, and what the entry stub needs to know. */
struct rk_exec_image {
	vaddr_t entry;
	vaddr_t stack_top;      /* highest usable byte + 1 */
	vaddr_t brk;            /* first byte past the last segment */
	vaddr_t load_bias;      /* nonzero only for ET_DYN */
	size_t  npages;
};

struct address_space;
struct task;

/* Parses and maps an image into `as`, which must be the address space this
 * thread is currently running in - the loader writes the segment contents
 * through their final user addresses rather than through a second mapping. */
int rk_elf_load(struct address_space *as, const void *data, size_t len,
                struct rk_exec_image *out);

/* Validates a header without mapping anything. Cheap, and the shell uses it to
 * say why a file is not runnable before committing an address space to it. */
int rk_elf_check(const void *data, size_t len);

/* Reads `path`, builds a task around it, and starts it in ring 3. The returned
 * task is live; it may already have exited by the time this returns. */
int rk_exec_spawn(const char *path, int argc, const char *const argv[],
                  struct task **out);

/* How much of a user stack a new process gets. Where it lives, and the bounds
 * a loadable segment must fall inside, are architecture policy and live in
 * <rk/mm.h> beside the reasons they differ. */
#define RK_USER_STACK_SIZE  (256 * 1024)
#define RK_USER_LOAD_MIN    RK_USER_VA_MIN
#define RK_USER_MAX         RK_USER_VA_MAX
