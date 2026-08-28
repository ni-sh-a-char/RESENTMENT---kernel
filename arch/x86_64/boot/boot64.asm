; SPDX-License-Identifier: Apache-2.0
; RESENTMENT kernel - 64-bit entry.
;
; Reached through the high alias, so from here on every symbol is virtual.
; The one job left is to move off the low boot stack onto a stack in .bss, so
; that the identity mapping can be torn down as soon as the VMM is up.

global long_mode_entry
extern x86_start

section .text
bits 64

long_mode_entry:
	mov rsp, kernel_boot_stack_top
	xor rbp, rbp                  ; terminate the frame chain for backtraces
	push rbp
	push rbp
	mov  rbp, rsp

	; rdi already holds the Multiboot2 information pointer.
	call x86_start

	; x86_start does not return.
	cli
.hang:
	hlt
	jmp .hang

section .bss
align 16
global kernel_boot_stack_bottom
kernel_boot_stack_bottom:
	resb 65536
global kernel_boot_stack_top
kernel_boot_stack_top:
