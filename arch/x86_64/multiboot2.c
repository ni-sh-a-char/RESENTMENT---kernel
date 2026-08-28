/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - Multiboot2 information parser.
 *
 * Translates whatever GRUB handed us into the machine-independent boot_info in
 * rk/boot.h. Everything above arch/ sees the same structure whether it came
 * from Multiboot2, a device tree or UEFI.
 */
#include <rk/boot.h>
#include <rk/string.h>
#include <rk/compiler.h>
#include <arch/x86.h>

struct mb2_tag {
	u32 type;
	u32 size;
} __packed;

struct mb2_mmap_entry {
	u64 addr;
	u64 len;
	u32 type;
	u32 zero;
} __packed;

struct mb2_tag_mmap {
	u32 type, size, entry_size, entry_version;
	struct mb2_mmap_entry entries[];
} __packed;

struct mb2_tag_fb {
	u32 type, size;
	u64 addr;
	u32 pitch, width, height;
	u8  bpp, fb_type, reserved;
	/* For fb_type 1 (RGB) the colour info follows. */
	u8  red_shift, red_size, green_shift, green_size, blue_shift, blue_size;
} __packed;

struct mb2_tag_module {
	u32 type, size, start, end;
	char cmdline[];
} __packed;

struct mb2_tag_string {
	u32 type, size;
	char string[];
} __packed;

struct mb2_tag_rsdp {
	u32 type, size;
	u8  rsdp[];
} __packed;

#define MB2_TAG_END       0
#define MB2_TAG_CMDLINE   1
#define MB2_TAG_BOOTLOADER 2
#define MB2_TAG_MODULE    3
#define MB2_TAG_BASICMEM  4
#define MB2_TAG_MMAP      6
#define MB2_TAG_FB        8
#define MB2_TAG_ACPI_OLD  14
#define MB2_TAG_ACPI_NEW  15

/* Multiboot memory types are almost but not quite ours, so map explicitly
 * rather than casting and hoping. */
static u32 mb2_mem_type(u32 t)
{
	switch (t) {
	case 1: return RK_MEM_USABLE;
	case 3: return RK_MEM_ACPI_RECLAIM;
	case 4: return RK_MEM_ACPI_NVS;
	case 5: return RK_MEM_BAD;
	default: return RK_MEM_RESERVED;
	}
}

extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

int x86_multiboot2_parse(u64 info_phys, struct boot_info *out)
{
	memset(out, 0, sizeof(*out));
	out->magic   = RK_BOOT_MAGIC;
	out->version = RK_BOOT_VERSION;
	strlcpy(out->platform, "pc", sizeof(out->platform));
	strlcpy(out->firmware, "multiboot2", sizeof(out->firmware));

	out->kernel_phys_start = (paddr_t)(uintptr_t)__kernel_phys_start;
	out->kernel_phys_end   = (paddr_t)(uintptr_t)__kernel_phys_end;

	if (!info_phys)
		return -1;

	/* The info block is below 4 GiB and the direct map is already live. */
	const u8 *base = (const u8 *)(uintptr_t)(DIRECT_MAP_BASE + info_phys);
	u32 total = *(const u32 *)base;
	const u8 *p = base + 8;
	const u8 *end = base + total;

	while (p + sizeof(struct mb2_tag) <= end) {
		const struct mb2_tag *tag = (const struct mb2_tag *)p;
		if (tag->type == MB2_TAG_END)
			break;

		switch (tag->type) {
		case MB2_TAG_CMDLINE: {
			const struct mb2_tag_string *s = (const void *)tag;
			strlcpy(out->cmdline, s->string, sizeof(out->cmdline));
			break;
		}
		case MB2_TAG_BOOTLOADER: {
			const struct mb2_tag_string *s = (const void *)tag;
			strlcpy(out->firmware, s->string, sizeof(out->firmware));
			break;
		}
		case MB2_TAG_MMAP: {
			const struct mb2_tag_mmap *m = (const void *)tag;
			u32 n = (m->size - sizeof(*m)) / m->entry_size;
			for (u32 i = 0; i < n && out->mmap_count < RK_MAX_MEMMAP; i++) {
				const struct mb2_mmap_entry *e =
					(const void *)((const u8 *)m->entries + (size_t)i * m->entry_size);
				if (!e->len)
					continue;
				out->mmap[out->mmap_count].base  = e->addr;
				out->mmap[out->mmap_count].len   = e->len;
				out->mmap[out->mmap_count].type  = mb2_mem_type(e->type);
				out->mmap[out->mmap_count].flags = 0;
				out->mmap_count++;
			}
			break;
		}
		case MB2_TAG_FB: {
			const struct mb2_tag_fb *f = (const void *)tag;
			out->fb.addr   = f->addr;
			out->fb.pitch  = f->pitch;
			out->fb.width  = f->width;
			out->fb.height = f->height;
			out->fb.bpp    = f->bpp;
			if (f->fb_type == 1) {
				out->fb.format      = RK_FB_RGB;
				out->fb.red_shift   = f->red_shift;
				out->fb.red_size    = f->red_size;
				out->fb.green_shift = f->green_shift;
				out->fb.green_size  = f->green_size;
				out->fb.blue_shift  = f->blue_shift;
				out->fb.blue_size   = f->blue_size;
			} else if (f->fb_type == 2) {
				out->fb.format = RK_FB_TEXT;
			} else {
				/* Indexed colour. The console cannot use a palette it did not
				 * set, so treat it as absent and fall back to serial. */
				out->fb.format = RK_FB_NONE;
			}
			break;
		}
		case MB2_TAG_MODULE: {
			const struct mb2_tag_module *m = (const void *)tag;
			if (out->module_count < RK_MAX_MODULES) {
				struct rk_module *mod = &out->modules[out->module_count++];
				mod->start = m->start;
				mod->end   = m->end;
				strlcpy(mod->name, m->cmdline, sizeof(mod->name));
				/* By convention the first module is the initial ramdisk. */
				if (!out->initrd_start) {
					out->initrd_start = m->start;
					out->initrd_end   = m->end;
				}
			}
			break;
		}
		case MB2_TAG_ACPI_OLD:
		case MB2_TAG_ACPI_NEW: {
			const struct mb2_tag_rsdp *r = (const void *)tag;
			out->acpi_rsdp = (paddr_t)(uintptr_t)r->rsdp - DIRECT_MAP_BASE;
			break;
		}
		default:
			break;
		}

		p += ALIGN_UP(tag->size, 8u);
	}

	/* A machine with no memory map at all is not one we can safely run on;
	 * say so here rather than failing mysteriously inside the allocator. */
	if (out->mmap_count == 0)
		return -1;
	return 0;
}
