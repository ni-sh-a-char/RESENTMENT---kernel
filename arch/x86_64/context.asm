; SPDX-License-Identifier: Apache-2.0
; RESENTMENT kernel - context switch.
;
; Only the callee-saved registers are switched. Everything else is already dead
; across a call boundary by the SysV ABI, so saving it would be pure cost - and
; this runs on every scheduling decision the system makes.
;
; The saved layout, from the new stack pointer upward, is:
;   rflags, r15, r14, r13, r12, rbx, rbp, return address
; arch_thread_init in thread.c builds exactly this by hand for a new thread.

global arch_context_switch
global x86_thread_trampoline
extern x86_thread_entry

section .text
bits 64

; void arch_context_switch(void **save_sp, void *load_sp)
arch_context_switch:
	push rbp
	push rbx
	push r12
	push r13
	push r14
	push r15
	pushfq

	mov [rdi], rsp          ; save the outgoing stack pointer
	mov rsp, rsi            ; adopt the incoming one

	popfq
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbx
	pop rbp
	ret                     ; into whatever that stack was about to return to

; A new thread lands here with its entry point in r12 and its argument in r13.
; It never returns: x86_thread_entry calls thread_exit when the body finishes.
x86_thread_trampoline:
	mov rdi, r12            ; entry
	mov rsi, r13            ; arg
	xor rbp, rbp            ; end the frame chain so backtraces terminate
	call x86_thread_entry
	ud2                     ; unreachable; trap loudly if it ever is
