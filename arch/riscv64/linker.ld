/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - riscv64 link layout.
 *
 * 0x80200000 is where OpenSBI hands control to the supervisor payload on the
 * QEMU virt machine and on every board that uses the standard firmware, which
 * is effectively all of them.
 */

ENTRY(_start)

SECTIONS
{
	. = 0x80200000;
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
		/* gp addresses the small-data area with a signed 12-bit offset, so it
		 * points 2 KiB into it to make both halves reachable. boot.S loads
		 * this symbol, and it has to exist or the relocation is unsatisfiable. */
		. = ALIGN(8);
		PROVIDE(__global_pointer$ = . + 0x800);
		*(.data)
		*(.data.*)
		*(.sdata)
		*(.sdata.*)
	}

	.bss ALIGN(4K) (NOLOAD) :
	{
		__bss_start = .;
		*(COMMON)
		*(.bss)
		*(.bss.*)
		*(.sbss)
		*(.sbss.*)
		. = ALIGN(4K);
		__bss_end = .;
	}

	. = ALIGN(4K);
	__kernel_end = .;
	__kernel_phys_end = .;

	/DISCARD/ : { *(.eh_frame) *(.note.*) *(.comment) }
}
