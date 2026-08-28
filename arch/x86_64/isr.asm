; SPDX-License-Identifier: Apache-2.0
; RESENTMENT kernel - interrupt stubs.
;
; 256 entry points that normalise the CPU's two different stack layouts (some
; exceptions push an error code, most do not) into one trapframe, then call a
; single C dispatcher. Generating them with a macro rather than by hand is not
; laziness: a hand-written table is where the off-by-one lives.

extern x86_trap_dispatch
global x86_isr_table

section .text
bits 64

; Save the general purpose registers in the order struct trapframe declares
; them, so C can index the frame instead of guessing.
%macro PUSH_REGS 0
	push rax
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
%endmacro

%macro POP_REGS 0
	pop r15
	pop r14
	pop r13
	pop r12
	pop r11
	pop r10
	pop r9
	pop r8
	pop rbp
	pop rdi
	pop rsi
	pop rdx
	pop rcx
	pop rbx
	pop rax
%endmacro

%macro ISR_NOERR 1
isr_stub_%1:
	push qword 0              ; dummy error code, so the frame is uniform
	push qword %1
	jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
	; the CPU already pushed an error code
	push qword %1
	jmp isr_common
%endmacro

isr_common:
	PUSH_REGS

	; If we came from ring 3, swap in the kernel GS base. Checking the saved
	; CS is the only reliable test; the alternative, a per-CPU flag, races
	; with the very interrupts this is meant to handle.
	mov ax, [rsp + 15*8 + 2*8 + 8]   ; saved cs
	and ax, 3
	jz .kernel_entry
	swapgs
.kernel_entry:

	cld
	mov rdi, rsp                     ; struct trapframe *
	call x86_trap_dispatch

	mov ax, [rsp + 15*8 + 2*8 + 8]
	and ax, 3
	jz .kernel_exit
	swapgs
.kernel_exit:

	POP_REGS
	add rsp, 16                      ; discard vector and error code
	iretq

; Vectors that push an error code: 8, 10..14, 17, 21, 29, 30.
%assign i 0
%rep 256
  %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
	ISR_ERR i
  %else
	ISR_NOERR i
  %endif
  %assign i i+1
%endrep

section .rodata
align 8
x86_isr_table:
%assign i 0
%rep 256
	dq isr_stub_%+i
	%assign i i+1
%endrep
