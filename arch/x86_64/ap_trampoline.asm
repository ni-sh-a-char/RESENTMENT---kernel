; SPDX-License-Identifier: Apache-2.0
; RESENTMENT kernel - application processor trampoline.
;
; An x86 core wakes up in 16-bit real mode at a page-aligned address below
; 1 MiB, with nothing configured. This blob is what carries it from there to
; 64-bit long mode running kernel C code, and it has to be position-independent
; because the kernel copies it wherever it found a free low page.
;
; The parameters - which page table to use, which stack to take, where to jump
; and which slot to report into - are written into the tail of this blob by the
; boot processor before it sends the startup interrupt. Passing them in memory
; rather than in registers is the only option: the startup interrupt carries
; nothing but the page number to begin executing at.

%define TRAMPOLINE_BASE 0x8000

global x86_ap_trampoline_start
global x86_ap_trampoline_end
extern x86_ap_entry

section .rodata
align 4096

x86_ap_trampoline_start:

bits 16
ap_start:
	cli
	cld

	; Segments are whatever the reset state left them; make them flat and
	; addressable relative to where this blob was copied.
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax

	; Straight to protected mode. There is nothing useful to do in real mode
	; and every instruction spent there is one that can go wrong.
	lgdt [TRAMPOLINE_BASE + (ap_gdt_ptr - x86_ap_trampoline_start)]

	mov eax, cr0
	or  eax, 1
	mov cr0, eax

	jmp dword 0x08:(TRAMPOLINE_BASE + (ap_protected - x86_ap_trampoline_start))

bits 32
ap_protected:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	; The same page tables the boot processor is already using. Sharing them
	; is the point: an application processor that built its own would see a
	; different machine.
	mov eax, [TRAMPOLINE_BASE + (ap_cr3 - x86_ap_trampoline_start)]
	mov cr3, eax

	mov eax, cr4
	or  eax, 1 << 5               ; PAE
	or  eax, 1 << 7               ; PGE
	mov cr4, eax

	mov ecx, 0xC0000080           ; EFER
	rdmsr
	or  eax, 1 << 8               ; LME
	or  eax, 1 << 11              ; NXE
	wrmsr

	mov eax, cr0
	or  eax, 1 << 16              ; WP
	or  eax, 1 << 31              ; PG
	mov cr0, eax

	jmp 0x18:(TRAMPOLINE_BASE + (ap_long - x86_ap_trampoline_start))

bits 64
ap_long:
	xor ax, ax
	mov ss, ax
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax

	mov rsp, [TRAMPOLINE_BASE + (ap_stack - x86_ap_trampoline_start)]
	xor rbp, rbp

	; Report in, so the boot processor knows this core is alive rather than
	; waiting out a timeout and guessing.
	mov qword [TRAMPOLINE_BASE + (ap_alive - x86_ap_trampoline_start)], 1

	; Into the kernel proper, at its high virtual address.
	mov rax, [TRAMPOLINE_BASE + (ap_entry - x86_ap_trampoline_start)]
	call rax

.hang:
	cli
	hlt
	jmp .hang

; --------------------------------------------------------------------- data

align 16
ap_gdt:
	dq 0
	dq 0x00CF9A000000FFFF         ; 0x08 32-bit code
	dq 0x00CF92000000FFFF         ; 0x10 32-bit data
	dq 0x00AF9A000000FFFF         ; 0x18 64-bit code
ap_gdt_ptr:
	dw ap_gdt_ptr - ap_gdt - 1
	dd TRAMPOLINE_BASE + (ap_gdt - x86_ap_trampoline_start)

; Filled in by the boot processor before the startup interrupt is sent.
align 8
ap_cr3:   dq 0
ap_stack: dq 0
ap_entry: dq 0
ap_alive: dq 0

x86_ap_trampoline_end:
