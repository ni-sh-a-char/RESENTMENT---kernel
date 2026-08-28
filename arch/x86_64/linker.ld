/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - x86_64 link layout.
 *
 * The kernel is a higher-half image: it is loaded at 1 MiB physical, where a
 * Multiboot2 loader can reach it, but every symbol above the boot stub is
 * linked at -2 GiB virtual. That leaves the entire lower canonical half free
 * for user address spaces, which is what makes a real user/kernel split
 * possible rather than a shared identity map.
 *
 * The .boot section is deliberately left at its physical address: the 32-bit
 * entry code runs before paging exists, so it cannot use high addresses.
 */

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x0000000000100000;

ENTRY(_start)

SECTIONS
{
	. = KERNEL_LMA;
	__kernel_phys_start = .;

	.boot ALIGN(4K) :
	{
		KEEP(*(.multiboot_header))
		*(.boot)
		*(.boot.*)
	}

	/* Boot page tables and the boot stack. NOLOAD keeps them out of the image
	 * but inside the same load segment, so the loader zeroes them for us; the
	 * boot stub zeroes them again rather than trusting that. */
	.boot.bss ALIGN(4K) (NOLOAD) :
	{
		__boot_bss_start = .;
		*(.boot.bss)
		. = ALIGN(4K);
		__boot_bss_end = .;
	}

	. = ALIGN(4K);
	__boot_end_phys = .;

	/* Everything from here on is addressed high. */
	. += KERNEL_VMA;

	.text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
	{
		__text_start = .;
		*(.text.boot)
		*(.text)
		*(.text.*)
		__text_end = .;
	}

	.rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
	{
		__rodata_start = .;
		*(.rodata)
		*(.rodata.*)
		. = ALIGN(8);
		__rk_initcall_start = .;
		KEEP(*(.rk_initcall))
		__rk_initcall_end = .;
		__rodata_end = .;
	}

	.data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
	{
		__data_start = .;
		*(.data)
		*(.data.*)
		__data_end = .;
	}

	.bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
	{
		__bss_start = .;
		*(COMMON)
		*(.bss)
		*(.bss.*)
		. = ALIGN(4K);
		__bss_end = .;
	}

	. = ALIGN(4K);
	__kernel_end = .;
	__kernel_phys_end = . - KERNEL_VMA;

	/DISCARD/ :
	{
		*(.eh_frame)
		*(.eh_frame_hdr)
		*(.note.*)
		*(.comment)
	}
}
