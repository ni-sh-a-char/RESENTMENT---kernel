/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT - the first program that runs in ring 3.
 *
 * This exists to prove a specific claim: that the kernel can take an ELF file
 * off a filesystem, build an address space for it, drop to the least
 * privileged mode the hardware offers, and come back through a system call.
 * Everything it prints is something the kernel had to do correctly for the
 * line to appear at all.
 *
 * It is deliberately not a shell and deliberately not linked against
 * anything. The interesting part of a capability system is what a program
 * *cannot* do, and a program with an empty capability space that still runs is
 * a better demonstration of that than one carrying a runtime with it.
 */
#include "rksys.h"

static void put(const char *s) { rk_puts(s); }

static void put_num(const char *label, unsigned long v, const char *suffix)
{
	char buf[24];
	put(label);
	put(rk_utoa(v, buf));
	put(suffix);
}

/* argc and argv arrive on the stack in the System V prefix layout the kernel
 * builds; see build_stack() in kernel/exec/elf.c. */
int rk_main(long argc, char **argv)
{
	put("\n");
	put("  hello from ring 3.\n");
	put("\n");

	put_num("  pid            ", (unsigned long)rk_getpid(), "\n");

	long t0 = rk_time_ns();
	put_num("  uptime         ", (unsigned long)(t0 / 1000000), " ms\n");
	put_num("  wall clock     ", (unsigned long)rk_wallclock(), " unix\n");

	/* Entropy from the kernel's CSPRNG, printed as the first two bytes so the
	 * line differs between boots and cannot be a constant somebody baked in. */
	unsigned char r[4] = { 0, 0, 0, 0 };
	if (rk_random(r, sizeof(r)) > 0) {
		char b[24];
		put("  entropy        ");
		put(rk_utoa(((unsigned long)r[0] << 8) | r[1], b));
		put("\n");
	}

	put_num("  argc           ", (unsigned long)argc, "\n");
	for (long i = 0; i < argc && i < 4; i++) {
		put("  argv           ");
		put(argv[i]);
		put("\n");
	}

	/* A voluntary trip through the scheduler, from user mode, and back. If the
	 * return path is wrong this is where it shows. */
	rk_yield();

	long t1 = rk_time_ns();
	put_num("  syscalls cost  ", (unsigned long)(t1 - t0), " ns for the above\n");

	put("\n");
	put("  Everything above required a working ELF loader, an address space,\n");
	put("  a ring transition and a system call return path. This process holds\n");
	put("  no capabilities: it can compute, print and exit, and nothing else.\n");
	put("\n");

	return 0;
}

/* The entry point the kernel jumps to. The stack pointer is exactly as
 * build_stack() left it, so argc is at the top and argv follows - which is why
 * this has to be assembly rather than a C function with parameters: C would
 * expect them in registers. */
#if defined(__x86_64__)
__asm__(
	".section .text.start\n"
	".global _start\n"
	"_start:\n"
	"  xor  %rbp, %rbp\n"
	"  mov  (%rsp), %rdi\n"        /* argc */
	"  lea  8(%rsp), %rsi\n"       /* argv */
	"  and  $-16, %rsp\n"
	"  call rk_main\n"
	"  mov  %eax, %edi\n"
	"  xor  %eax, %eax\n"          /* SYS_EXIT */
	"  syscall\n"
	"1:jmp 1b\n");
#elif defined(__aarch64__)
__asm__(
	".section .text.start\n"
	".global _start\n"
	"_start:\n"
	"  ldr  x0, [sp]\n"            /* argc */
	"  add  x1, sp, #8\n"          /* argv */
	"  mov  x2, sp\n"              /* SP is not a valid AND destination */
	"  and  x2, x2, #-16\n"
	"  mov  sp, x2\n"
	"  bl   rk_main\n"
	"  mov  x8, #0\n"              /* SYS_EXIT */
	"  svc  #0\n"
	"1:b 1b\n");
#elif defined(__riscv)
__asm__(
	".section .text.start\n"
	".global _start\n"
	"_start:\n"
	"  ld   a0, 0(sp)\n"           /* argc */
	"  addi a1, sp, 8\n"           /* argv */
	"  andi sp, sp, -16\n"
	"  call rk_main\n"
	"  li   a7, 0\n"               /* SYS_EXIT */
	"  ecall\n"
	"1:j 1b\n");
#endif
