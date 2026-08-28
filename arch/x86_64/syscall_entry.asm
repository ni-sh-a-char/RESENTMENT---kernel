; SPDX-License-Identifier: Apache-2.0
; RESENTMENT kernel - SYSCALL entry.
;
; SYSCALL is fast because it does almost nothing: it saves rip in rcx and
; rflags in r11, loads cs and ss from an MSR, and jumps. Everything else -
; the stack switch above all - is our responsibility, and getting the order
; wrong means running kernel code on a user-controlled stack.

global x86_syscall_entry
extern x86_syscall_handler
extern x86_percpu_kstack

section .text
bits 64

x86_syscall_entry:
	; The user stack is not trustworthy. Swap to the kernel GS first, then
	; take the kernel stack out of the per-CPU block.
	swapgs
	mov [gs:24], rsp              ; stash the user rsp in percpu.scratch
	mov rsp, [gs:16]              ; percpu.kstack_top

	push qword [gs:24]            ; user rsp, so we can restore it
	push r11                      ; user rflags
	push rcx                      ; user rip

	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15

	; SysV wants the argument block in rdi. Build it on the stack in the
	; order struct rk_syscall_args declares.
	push r9
	push r8
	push r10
	push rdx
	push rsi
	push rdi
	push rax
	mov rdi, rsp

	call x86_syscall_handler

	; Restore the argument registers rather than discarding them. SYSCALL
	; itself only destroys rcx and r11, so that is all a caller is required to
	; treat as clobbered - it is free to keep a live value in rdi or rsi
	; across the call, and a great deal of ordinary code does. Dropping the
	; block with `add rsp, 7*8` left those registers holding whatever the
	; handler happened to leave there, which corrupts the caller in a way that
	; depends on optimisation level and is therefore very hard to attribute.
	;
	; rax is skipped: the handler's return value is already in it.
	add rsp, 8
	pop rdi
	pop rsi
	pop rdx
	pop r10
	pop r8
	pop r9

	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx

	pop rcx                       ; user rip
	pop r11                       ; user rflags
	pop rsp                       ; user rsp

	swapgs
	o64 sysret
