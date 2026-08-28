; SPDX-License-Identifier: Apache-2.0
; RESENTMENT kernel - 32-bit boot stub.
;
; Runs at its physical address with paging off. Its job is to prove the machine
; can run the kernel, build a page table that maps three things, and get into
; long mode:
;
;   identity     first 1 GiB, so the instruction after enabling paging is still
;                fetchable. Torn down by the VMM once we are running high.
;   direct map   4 GiB of physical RAM at 0xFFFF800000000000, so the kernel can
;                read any physical page without building a temporary mapping.
;   kernel       first 1 GiB at 0xFFFFFFFF80000000, where the image is linked.
;
; Everything uses 2 MiB pages. 1 GiB pages would need fewer entries but are not
; universal, and this code has to run on whatever the user actually has.
;
; Note on addresses: the .boot and .boot.bss sections are linked at their
; physical addresses precisely so that this file needs no virtual-to-physical
; arithmetic. Only symbols in .text are high, and the only one referenced here
; is long_mode_entry, which is jumped to after paging is on.

PAGE_PRESENT equ 1 << 0
PAGE_WRITE   equ 1 << 1
PAGE_HUGE    equ 1 << 7

global _start
global boot_stack_top
extern long_mode_entry

section .boot
bits 32

_start:
	cli
	cld
	mov esp, boot_stack_top

	; Multiboot2 hands us the magic in eax and the info pointer in ebx.
	mov edi, eax
	mov esi, ebx

	call check_multiboot
	call check_cpuid
	call check_long_mode

	call zero_page_tables
	call build_page_tables
	call enable_paging

	lgdt [gdt64.pointer]
	jmp  gdt64.code:long_mode_trampoline

; ---------------------------------------------------------------- checks

; Either loader is acceptable. Which one it was decides how the information
; block is parsed, so the magic is carried through to C rather than discarded.
check_multiboot:
	cmp edi, 0x36D76289             ; Multiboot2
	je  .ok
	cmp edi, 0x2BADB002             ; Multiboot1
	je  .ok
	mov al, 'M'
	jmp boot_error
.ok:
	ret

check_cpuid:
	; CPUID exists if bit 21 of EFLAGS can be toggled.
	pushfd
	pop eax
	mov ecx, eax
	xor eax, 1 << 21
	push eax
	popfd
	pushfd
	pop eax
	push ecx
	popfd
	cmp eax, ecx
	je .fail
	ret
.fail:
	mov al, 'C'
	jmp boot_error

check_long_mode:
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000001
	jb .fail
	mov eax, 0x80000001
	cpuid
	test edx, 1 << 29             ; LM
	jz .fail
	ret
.fail:
	mov al, 'L'
	jmp boot_error

; ------------------------------------------------------------ page tables

; Only the tables are cleared. The boot stack lives past boot_pt_end and is
; holding this call's return address.
zero_page_tables:
	mov edi, boot_pt_start
	mov ecx, boot_pt_end - boot_pt_start
	shr ecx, 2
	xor eax, eax
	rep stosd
	ret

build_page_tables:
	; --- four page directories, 2048 entries, 4 GiB of 2 MiB pages ------
	mov edi, pd_tables
	mov eax, PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE
	xor ecx, ecx
.pd_loop:
	mov [edi], eax                ; low dword: flags plus phys bits 31:21
	mov dword [edi + 4], 0        ; high dword: phys bits 51:32, all zero
	add eax, 0x200000
	add edi, 8
	inc ecx
	cmp ecx, 512 * 4
	jne .pd_loop

	; --- identity PDPT: entry 0 -> pd0 -----------------------------------
	mov eax, pd_tables
	or  eax, PAGE_PRESENT | PAGE_WRITE
	mov [pdpt_low], eax
	mov dword [pdpt_low + 4], 0

	; --- direct-map PDPT: entries 0..3 -> pd0..pd3 -----------------------
	mov edi, pdpt_direct
	mov eax, pd_tables
	or  eax, PAGE_PRESENT | PAGE_WRITE
	mov ecx, 4
.direct_loop:
	mov [edi], eax
	mov dword [edi + 4], 0
	add eax, 4096
	add edi, 8
	dec ecx
	jnz .direct_loop

	; --- kernel PDPT: entry 510 covers 0xFFFFFFFF80000000 ----------------
	mov eax, pd_tables
	or  eax, PAGE_PRESENT | PAGE_WRITE
	mov [pdpt_high + 510 * 8], eax
	mov dword [pdpt_high + 510 * 8 + 4], 0

	; --- PML4 -------------------------------------------------------------
	mov eax, pdpt_low
	or  eax, PAGE_PRESENT | PAGE_WRITE
	mov [pml4], eax                    ; 0x0000000000000000  identity

	mov eax, pdpt_direct
	or  eax, PAGE_PRESENT | PAGE_WRITE
	mov [pml4 + 256 * 8], eax          ; 0xFFFF800000000000  direct map

	; Recursive slot: lets the 64-bit VMM edit any page table through a
	; fixed virtual window instead of mapping each level temporarily.
	mov eax, pml4
	or  eax, PAGE_PRESENT | PAGE_WRITE
	mov [pml4 + 510 * 8], eax          ; 0xFFFFFF0000000000  self

	mov eax, pdpt_high
	or  eax, PAGE_PRESENT | PAGE_WRITE
	mov [pml4 + 511 * 8], eax          ; 0xFFFFFF8000000000  kernel
	ret

enable_paging:
	mov eax, pml4
	mov cr3, eax

	mov eax, cr4
	or  eax, 1 << 5               ; PAE
	or  eax, 1 << 7               ; PGE, global pages
	mov cr4, eax

	mov ecx, 0xC0000080           ; EFER
	rdmsr
	or  eax, 1 << 8               ; LME, long mode enable
	or  eax, 1 << 11              ; NXE, no-execute enable
	wrmsr

	mov eax, cr0
	or  eax, 1 << 16              ; WP, honour read-only even in ring 0
	or  eax, 1 << 31              ; PG
	mov cr0, eax
	ret

; ----------------------------------------------------------------- errors

; Writes "ERR: x" to the top-left of VGA text memory. It is the only
; diagnostic available before a console exists, and on a machine that fails
; here it is the difference between a bug report and a blank screen.
boot_error:
	mov dword [0xB8000], 0x4F524F45
	mov dword [0xB8004], 0x4F3A4F52
	mov dword [0xB8008], 0x4F204F20
	mov byte  [0xB800A], al
.hang:
	hlt
	jmp .hang

; ------------------------------------------------------- long mode entry

bits 64
long_mode_trampoline:
	; Segment registers are mostly ignored in long mode but must be sane.
	xor ax, ax
	mov ss, ax
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax

	; SysV arguments: the information pointer first, then the loader magic,
	; so C can tell a Multiboot1 block from a Multiboot2 one.
	mov eax, edi                  ; magic
	mov edi, esi                  ; info pointer  -> first argument
	mov esi, eax                  ; magic         -> second argument

	; Both mappings are live, so switch to the high alias of the kernel.
	mov rax, long_mode_entry
	jmp rax

; ------------------------------------------------------------------ data

section .boot
align 16
gdt64:
	dq 0
.code: equ $ - gdt64
	dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)   ; 64-bit ring 0 code
.data: equ $ - gdt64
	dq (1 << 41) | (1 << 44) | (1 << 47)               ; ring 0 data
.pointer:
	dw $ - gdt64 - 1
	dd gdt64
	dd 0

; nobits: reserved, not stored in the image. The loader zeroes it as part
; of the load segment, and zero_page_tables clears the tables again anyway.
section .boot.bss nobits alloc noexec write align=4096
boot_pt_start:
pml4:        resb 4096
pdpt_low:    resb 4096
pdpt_direct: resb 4096
pdpt_high:   resb 4096
pd_tables:   resb 4096 * 4       ; pd0..pd3, contiguous by design
boot_pt_end:

boot_stack_bottom:
	resb 4096 * 16               ; 64 KiB, enough for early init and no more
boot_stack_top:
