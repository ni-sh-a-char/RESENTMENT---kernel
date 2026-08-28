/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - Multiboot 1 information parser.
 *
 * The older handover format, kept because QEMU's own -kernel loader and every
 * bootloader older than GRUB2 speak it and nothing else. It carries less than
 * Multiboot2 does - no ACPI pointer, a coarser framebuffer description - but
 * it carries the two things the boot path cannot proceed without: a memory map
 * and the modules.
 */
#include <rk/boot.h>
#include <rk/string.h>
#include <rk/compiler.h>
#include <arch/x86.h>

#define MB1_FLAG_MEM      (1u << 0)
#define MB1_FLAG_BOOTDEV  (1u << 1)
#define MB1_FLAG_CMDLINE  (1u << 2)
#define MB1_FLAG_MODS     (1u << 3)
#define MB1_FLAG_MMAP     (1u << 6)
#define MB1_FLAG_LOADER   (1u << 9)
#define MB1_FLAG_FB       (1u << 12)

struct mb1_info {
	u32 flags;
	u32 mem_lower, mem_upper;
	u32 boot_device;
	u32 cmdline;
	u32 mods_count, mods_addr;
	u32 syms[4];
	u32 mmap_length, mmap_addr;
	u32 drives_length, drives_addr;
	u32 config_table;
	u32 boot_loader_name;
	u32 apm_table;
	u32 vbe_control_info, vbe_mode_info;
	u16 vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
	u64 framebuffer_addr;
	u32 framebuffer_pitch, framebuffer_width, framebuffer_height;
	u8  framebuffer_bpp, framebuffer_type;
	u8  color_info[6];
} __packed;

struct mb1_mmap_entry {
	u32 size;          /* of the rest of this entry, not including itself */
	u64 addr;
	u64 len;
	u32 type;
} __packed;

struct mb1_module {
	u32 start, end, cmdline, pad;
} __packed;

static const void *lowmem(u32 phys)
{
	return (const void *)(uintptr_t)(DIRECT_MAP_BASE + phys);
}

int x86_multiboot1_parse(u64 info_phys, struct boot_info *out);

int x86_multiboot1_parse(u64 info_phys, struct boot_info *out)
{
	extern char __kernel_phys_start[], __kernel_phys_end[];

	memset(out, 0, sizeof(*out));
	out->magic   = RK_BOOT_MAGIC;
	out->version = RK_BOOT_VERSION;
	strlcpy(out->platform, "pc", sizeof(out->platform));
	strlcpy(out->firmware, "multiboot1", sizeof(out->firmware));
	out->kernel_phys_start = (paddr_t)(uintptr_t)__kernel_phys_start;
	out->kernel_phys_end   = (paddr_t)(uintptr_t)__kernel_phys_end;

	if (!info_phys)
		return -1;

	const struct mb1_info *mb = lowmem((u32)info_phys);

	if (mb->flags & MB1_FLAG_LOADER)
		strlcpy(out->firmware, lowmem(mb->boot_loader_name), sizeof(out->firmware));
	if (mb->flags & MB1_FLAG_CMDLINE)
		strlcpy(out->cmdline, lowmem(mb->cmdline), sizeof(out->cmdline));

	if (mb->flags & MB1_FLAG_MMAP) {
		/* Each entry is self-sizing, and the size field excludes itself,
		 * which is the detail that trips every first implementation. */
		u32 off = 0;
		while (off < mb->mmap_length && out->mmap_count < RK_MAX_MEMMAP) {
			const struct mb1_mmap_entry *e = lowmem(mb->mmap_addr + off);
			if (e->len) {
				out->mmap[out->mmap_count].base = e->addr;
				out->mmap[out->mmap_count].len  = e->len;
				out->mmap[out->mmap_count].type =
					e->type == 1 ? RK_MEM_USABLE :
					e->type == 3 ? RK_MEM_ACPI_RECLAIM :
					e->type == 4 ? RK_MEM_ACPI_NVS :
					e->type == 5 ? RK_MEM_BAD : RK_MEM_RESERVED;
				out->mmap_count++;
			}
			off += e->size + 4;
		}
	} else if (mb->flags & MB1_FLAG_MEM) {
		/* No map, only the two totals. Reconstruct the conventional PC
		 * layout from them, which is what those fields describe. */
		out->mmap[0].base = 0;
		out->mmap[0].len  = (u64)mb->mem_lower * 1024;
		out->mmap[0].type = RK_MEM_USABLE;
		out->mmap[1].base = 0x100000;
		out->mmap[1].len  = (u64)mb->mem_upper * 1024;
		out->mmap[1].type = RK_MEM_USABLE;
		out->mmap_count = 2;
	}

	if (mb->flags & MB1_FLAG_MODS) {
		const struct mb1_module *m = lowmem(mb->mods_addr);
		for (u32 i = 0; i < mb->mods_count && i < RK_MAX_MODULES; i++) {
			out->modules[out->module_count].start = m[i].start;
			out->modules[out->module_count].end   = m[i].end;
			if (m[i].cmdline)
				strlcpy(out->modules[out->module_count].name,
				        lowmem(m[i].cmdline),
				        sizeof(out->modules[0].name));
			if (!out->initrd_start) {
				out->initrd_start = m[i].start;
				out->initrd_end   = m[i].end;
			}
			out->module_count++;
		}
	}

	if (mb->flags & MB1_FLAG_FB) {
		out->fb.addr   = mb->framebuffer_addr;
		out->fb.pitch  = mb->framebuffer_pitch;
		out->fb.width  = mb->framebuffer_width;
		out->fb.height = mb->framebuffer_height;
		out->fb.bpp    = mb->framebuffer_bpp;
		if (mb->framebuffer_type == 1) {
			out->fb.format      = RK_FB_RGB;
			out->fb.red_shift   = mb->color_info[1];
			out->fb.red_size    = mb->color_info[0];
			out->fb.green_shift = mb->color_info[3];
			out->fb.green_size  = mb->color_info[2];
			out->fb.blue_shift  = mb->color_info[5];
			out->fb.blue_size   = mb->color_info[4];
		} else if (mb->framebuffer_type == 2) {
			out->fb.format = RK_FB_TEXT;
		}
	} else {
		/* No framebuffer tag at all means a plain text console, which is
		 * what QEMU gives a Multiboot1 kernel. */
		out->fb.format = RK_FB_TEXT;
	}

	return out->mmap_count ? 0 : -1;
}
