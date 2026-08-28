/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - flattened device tree.
 *
 * The ARM and RISC-V equivalent of the PC memory map, and the reason one
 * kernel image can boot on boards that share no hardware: the firmware
 * describes the machine, the kernel reads the description.
 *
 * A read-only parser, no allocation, no copying. It answers the questions the
 * boot path actually asks - where is memory, where is the console, what is the
 * initrd - and leaves everything else to the drivers that care.
 */
#include <rk/types.h>
#include <rk/boot.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/compiler.h>

#undef RK_SUBSYS
#define RK_SUBSYS "fdt"

#define FDT_MAGIC       0xD00DFEED
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

struct fdt_header {
	u32 magic;
	u32 totalsize;
	u32 off_dt_struct;
	u32 off_dt_strings;
	u32 off_mem_rsvmap;
	u32 version;
	u32 last_comp_version;
	u32 boot_cpuid_phys;
	u32 size_dt_strings;
	u32 size_dt_struct;
};

/* Everything in a device tree is big-endian, whatever the CPU is. */
static inline u32 be32(const void *p)
{
	const u8 *b = p;
	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static inline u64 be64(const void *p)
{
	return ((u64)be32(p) << 32) | be32((const u8 *)p + 4);
}

/* Cell values are one or two 32-bit words depending on the enclosing node's
 * #address-cells and #size-cells, which is the part everybody gets wrong. */
static u64 read_cells(const u8 **p, u32 cells)
{
	u64 v = 0;
	for (u32 i = 0; i < cells; i++) {
		v = (v << 32) | be32(*p);
		*p += 4;
	}
	return v;
}

struct fdt_walk {
	const struct fdt_header *hdr;
	const u8 *strings;
	const u8 *structs;
	const u8 *end;
};

static const char *prop_name(const struct fdt_walk *w, u32 nameoff)
{
	return (const char *)(w->strings + nameoff);
}

int rk_fdt_parse(u64 dtb_addr, struct boot_info *out);
u64 rk_fdt_locate(u64 hint, paddr_t search_base, size_t search_len);

/* Does this address hold a plausible device tree? Checking the magic alone
 * finds false positives in whatever happened to be in memory, so the size
 * fields have to agree with each other too. */
static bool looks_like_fdt(u64 addr)
{
	if (!addr || (addr & 3))
		return false;

	const struct fdt_header *h = (const struct fdt_header *)(uintptr_t)addr;
	if (be32(&h->magic) != FDT_MAGIC)
		return false;

	u32 total = be32(&h->totalsize);
	if (total < sizeof(*h) || total > (16u << 20))
		return false;
	if (be32(&h->off_dt_struct) >= total || be32(&h->off_dt_strings) >= total)
		return false;
	return be32(&h->version) >= 16 && be32(&h->version) <= 17;
}

/* Find the device tree.
 *
 * The firmware is supposed to hand its address over in a register, and on
 * RISC-V under SBI it does. QEMU booting a bare ELF on ARM does not, and
 * neither do several real boards, so rather than silently falling back to a
 * hardcoded memory layout - which is how a machine ends up quietly running
 * with a quarter of its RAM - the kernel goes and looks for it.
 */
u64 rk_fdt_locate(u64 hint, paddr_t search_base, size_t search_len)
{
	if (looks_like_fdt(hint))
		return hint;

	/* Firmware conventionally places the tree at the start of RAM, just
	 * below the kernel, or near the top of the first 128 MiB. Scanning on a
	 * 4 KiB grid covers all three without walking the whole address space. */
	for (size_t off = 0; off + sizeof(struct fdt_header) < search_len;
	     off += 0x1000) {
		u64 addr = (u64)search_base + off;
		if (looks_like_fdt(addr))
			return addr;
	}
	return 0;
}

int rk_fdt_parse(u64 dtb_addr, struct boot_info *out)
{
	if (!dtb_addr)
		return RK_ENODEV;

	const struct fdt_header *h = (const struct fdt_header *)(uintptr_t)dtb_addr;
	if (be32(&h->magic) != FDT_MAGIC) {
		/* Say where we looked and what we found. A silent fall back to a
		 * hardcoded layout is how a machine ends up quietly running with a
		 * fraction of its memory. */
		pr_warn("no device tree at %#llx (found %#010x, wanted %#010x)",
		        (unsigned long long)dtb_addr, be32(&h->magic), FDT_MAGIC);
		return RK_EINVAL;
	}

	u32 total = be32(&h->totalsize);
	if (total < sizeof(*h) || total > (64u << 20))
		return RK_EINVAL;

	struct fdt_walk w = {
		.hdr     = h,
		.strings = (const u8 *)h + be32(&h->off_dt_strings),
		.structs = (const u8 *)h + be32(&h->off_dt_struct),
	};
	w.end = w.structs + be32(&h->size_dt_struct);

	memset(out, 0, sizeof(*out));
	out->magic   = RK_BOOT_MAGIC;
	out->version = RK_BOOT_VERSION;
	out->device_tree = (paddr_t)dtb_addr;
	strlcpy(out->firmware, "device-tree", sizeof(out->firmware));
	strlcpy(out->platform, "unknown", sizeof(out->platform));

	/* Defaults from the specification, overridden by the root node. */
	u32 addr_cells = 2, size_cells = 1;
	char node[64] = "";
	int depth = 0;
	bool in_memory = false;
	bool in_chosen = false;
	/* Depth at which /reserved-memory was entered, or -1. Its children each
	 * carry a reg property naming a range the kernel must not touch - which
	 * on RISC-V is where OpenSBI is still executing, PMP-protected, so
	 * handing it to the page allocator produces an access fault the moment
	 * something writes there. */
	int reserved_depth = -1;
	u64 initrd_start = 0, initrd_end = 0;

	const u8 *p = w.structs;
	while (p + 4 <= w.end) {
		u32 tok = be32(p);
		p += 4;

		if (tok == FDT_NOP)
			continue;
		if (tok == FDT_END)
			break;

		if (tok == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			size_t len = strlen(name);
			p += ALIGN_UP(len + 1, 4u);
			depth++;
			strlcpy(node, name, sizeof(node));
			/* A memory node is named "memory" or "memory@<address>". */
			in_memory = (strncmp(name, "memory", 6) == 0 &&
			             (name[6] == '\0' || name[6] == '@'));
			in_chosen = (strcmp(name, "chosen") == 0);
			if (strcmp(name, "reserved-memory") == 0)
				reserved_depth = depth;
			continue;
		}

		if (tok == FDT_END_NODE) {
			if (reserved_depth >= 0 && depth <= reserved_depth)
				reserved_depth = -1;
			depth--;
			in_memory = in_chosen = false;
			continue;
		}

		if (tok != FDT_PROP)
			break;   /* malformed: stop rather than walk off the end */

		if (p + 8 > w.end)
			break;
		u32 plen = be32(p);
		u32 nameoff = be32(p + 4);
		p += 8;
		const u8 *val = p;
		if (val + plen > w.end)
			break;
		p += ALIGN_UP(plen, 4u);

		const char *pname = prop_name(&w, nameoff);

		if (depth == 1 && strcmp(pname, "#address-cells") == 0)
			addr_cells = be32(val);
		else if (depth == 1 && strcmp(pname, "#size-cells") == 0)
			size_cells = be32(val);
		else if (depth == 1 && strcmp(pname, "model") == 0)
			strlcpy(out->platform, (const char *)val, sizeof(out->platform));
		else if (in_chosen && strcmp(pname, "bootargs") == 0)
			strlcpy(out->cmdline, (const char *)val, sizeof(out->cmdline));
		else if (in_chosen && strcmp(pname, "linux,initrd-start") == 0)
			initrd_start = plen == 8 ? be64(val) : be32(val);
		else if (in_chosen && strcmp(pname, "linux,initrd-end") == 0)
			initrd_end = plen == 8 ? be64(val) : be32(val);
		else if (reserved_depth >= 0 && depth > reserved_depth &&
		         strcmp(pname, "reg") == 0) {
			const u8 *q = val;
			const u8 *qend = val + plen;
			while (q + (addr_cells + size_cells) * 4 <= qend &&
			       out->mmap_count < RK_MAX_MEMMAP) {
				u64 base = read_cells(&q, addr_cells);
				u64 size = read_cells(&q, size_cells);
				if (!size)
					continue;
				out->mmap[out->mmap_count].base = base;
				out->mmap[out->mmap_count].len  = size;
				out->mmap[out->mmap_count].type = RK_MEM_RESERVED;
				out->mmap_count++;
			}
		}
		else if (in_memory && strcmp(pname, "reg") == 0) {
			const u8 *q = val;
			const u8 *qend = val + plen;
			while (q + (addr_cells + size_cells) * 4 <= qend &&
			       out->mmap_count < RK_MAX_MEMMAP) {
				u64 base = read_cells(&q, addr_cells);
				u64 size = read_cells(&q, size_cells);
				if (!size)
					continue;
				out->mmap[out->mmap_count].base = base;
				out->mmap[out->mmap_count].len  = size;
				out->mmap[out->mmap_count].type = RK_MEM_USABLE;
				out->mmap_count++;
			}
		}
	}

	if (initrd_end > initrd_start) {
		out->initrd_start = initrd_start;
		out->initrd_end   = initrd_end;
	}

	/* Reserved memory regions come from a separate table, and honouring them
	 * is not optional: they typically hold the firmware still running. */
	const u8 *rsv = (const u8 *)h + be32(&h->off_mem_rsvmap);
	for (int i = 0; i < 16 && out->mmap_count < RK_MAX_MEMMAP; i++) {
		u64 base = be64(rsv);
		u64 size = be64(rsv + 8);
		rsv += 16;
		if (!base && !size)
			break;
		out->mmap[out->mmap_count].base = base;
		out->mmap[out->mmap_count].len  = size;
		out->mmap[out->mmap_count].type = RK_MEM_RESERVED;
		out->mmap_count++;
	}

	if (!out->mmap_count)
		return RK_ENODEV;

	u64 usable = 0;
	for (u32 i = 0; i < out->mmap_count; i++)
		if (out->mmap[i].type == RK_MEM_USABLE)
			usable += out->mmap[i].len;

	pr_info("device tree: %s, %u regions, %pB usable",
	        out->platform, out->mmap_count, RK_BYTES(usable));
	return RK_OK;
}
