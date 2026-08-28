/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - aarch64 link layout.
 *
 * Linked at the address the firmware loads to, because this port runs with the
 * MMU off and identity mapped for now. 0x40080000 is where U-Boot and QEMU
 * both place a kernel on the virt machine and on most ARM boards.
 */

ENTRY(_start)

SECTIONS
{
	. = 0x40080000;
	__kernel_phys_start = .;

	.text ALIGN(4K) :
	{
		KEEP(*(.text.boot))
		*(.text)
		*(.text.*)
	}

	.rodata ALIGN(4K) :
	{
		__rodata_start = .;
		*(.rodata)
		*(.rodata.*)
		__rodata_end = .;
	}

	.data ALIGN(4K) :
	{
		*(.data)
		*(.data.*)
	}

	.bss ALIGN(4K) (NOLOAD) :
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
	__kernel_phys_end = .;

	/DISCARD/ : { *(.eh_frame) *(.note.*) *(.comment) }
}
