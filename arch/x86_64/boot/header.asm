; SPDX-License-Identifier: Apache-2.0
; RESENTMENT kernel - Multiboot2 header.
;
; The framebuffer tag asks the loader for a linear RGB mode. If the firmware
; cannot provide one, GRUB falls back to text mode and sets the framebuffer
; type accordingly, which the console layer handles - so this works on a modern
; UEFI machine with no VGA hardware at all and on a 1998 PC alike.

MB2_MAGIC        equ 0xE85250D6
MB2_ARCH_I386    equ 0

; --- Multiboot 1 ------------------------------------------------------------
;
; Kept alongside the Multiboot2 header because the two are not alternatives in
; practice: GRUB2 prefers Multiboot2, while QEMU's own -kernel loader and older
; bootloaders only understand Multiboot1. Supporting both costs 12 bytes and a
; second parser, and means the same image boots from a menu entry, from an ISO,
; and straight off the QEMU command line.

MB1_MAGIC equ 0x1BADB002
MB1_FLAGS equ 0x00000003        ; page-align modules, supply memory info

section .multiboot_header
align 4
mb1_header:
	dd MB1_MAGIC
	dd MB1_FLAGS
	dd -(MB1_MAGIC + MB1_FLAGS)

align 8
header_start:
	dd MB2_MAGIC
	dd MB2_ARCH_I386
	dd header_end - header_start
	dd 0x100000000 - (MB2_MAGIC + MB2_ARCH_I386 + (header_end - header_start))

	; --- information request -------------------------------------------
align 8
req_tag_start:
	dw 1                       ; type: information request
	dw 0                       ; flags: 0 = required
	dd req_tag_end - req_tag_start
	dd 4                       ; basic meminfo
	dd 6                       ; memory map
	dd 8                       ; framebuffer info
	dd 3                       ; modules
	dd 1                       ; boot command line
	dd 14                      ; ACPI old RSDP
	dd 15                      ; ACPI new RSDP
	dd 0                       ; padding to 8-byte alignment
req_tag_end:

	; --- framebuffer request -------------------------------------------
align 8
fb_tag_start:
	dw 5                       ; type: framebuffer
	dw 1                       ; flags: 1 = optional, fall back to text
	dd fb_tag_end - fb_tag_start
	dd 1024                    ; preferred width
	dd 768                     ; preferred height
	dd 32                      ; preferred depth
fb_tag_end:

	; --- module alignment ----------------------------------------------
align 8
	dw 6                       ; type: module alignment
	dw 0
	dd 8

	; --- end tag --------------------------------------------------------
align 8
	dw 0
	dw 0
	dd 8
header_end:
