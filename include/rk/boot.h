/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - machine-independent boot description.
 *
 * Each architecture parses whatever its firmware hands it (Multiboot2 on PC,
 * a flattened device tree on ARM and RISC-V, UEFI memory maps on servers) and
 * fills in exactly this structure. The portable kernel never sees the source
 * format, which is what lets one kernel image target laptops, phones, boards
 * and microservers without conditionals sprinkled through the core.
 */
#pragma once

#include <rk/types.h>

#define RK_MAX_MEMMAP   128
#define RK_MAX_MODULES  16
#define RK_CMDLINE_MAX  512

enum rk_mem_type {
	RK_MEM_USABLE = 1,
	RK_MEM_RESERVED,
	RK_MEM_ACPI_RECLAIM,
	RK_MEM_ACPI_NVS,
	RK_MEM_BAD,
	RK_MEM_BOOTLOADER,   /* reclaimable once we are done with boot data */
	RK_MEM_KERNEL,
	RK_MEM_MODULE,
	RK_MEM_FRAMEBUFFER,
	RK_MEM_MMIO,
};

struct rk_memregion {
	paddr_t base;
	u64     len;
	u32     type;
	u32     flags;
};

struct rk_module {
	paddr_t     start;
	paddr_t     end;
	char        name[64];
};

enum rk_fb_format {
	RK_FB_NONE = 0,
	RK_FB_RGB,       /* packed, described by the mask fields */
	RK_FB_TEXT,      /* legacy VGA text cells */
};

struct rk_framebuffer {
	paddr_t addr;
	u32     width, height, pitch;
	u8      bpp;
	u8      format;
	u8      red_shift, red_size;
	u8      green_shift, green_size;
	u8      blue_shift, blue_size;
};

struct boot_info {
	u32  magic;
	u32  version;

	char cmdline[RK_CMDLINE_MAX];

	struct rk_memregion mmap[RK_MAX_MEMMAP];
	u32                 mmap_count;

	struct rk_module    modules[RK_MAX_MODULES];
	u32                 module_count;

	struct rk_framebuffer fb;

	paddr_t kernel_phys_start, kernel_phys_end;
	paddr_t initrd_start, initrd_end;

	paddr_t acpi_rsdp;      /* 0 if absent */
	paddr_t device_tree;    /* 0 if absent */
	paddr_t efi_system_table;

	s64  wallclock_unix;    /* firmware time, 0 if unknown */
	char platform[64];      /* e.g. "pc-q35", "qemu-virt", "raspi4" */
	char firmware[64];      /* e.g. "multiboot2", "uefi", "sbi" */
};

#define RK_BOOT_MAGIC   0x52534E54u  /* RSNT */
#define RK_BOOT_VERSION 1u

extern struct boot_info rk_boot_info;

const char *rk_mem_type_name(u32 type);

/* Parse key=value or bare flags out of the command line.
 * Returns NULL when absent; the pointer is into the cmdline buffer. */
const char *rk_cmdline_get(const char *key);
bool        rk_cmdline_flag(const char *key);
u64         rk_cmdline_u64(const char *key, u64 fallback);
