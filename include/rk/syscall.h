/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - system call interface.
 *
 * The ABI is capability-oriented: almost every call takes a handle as its
 * first argument and fails with RK_EACCES if that handle does not carry the
 * right. There is no call that grants authority out of thin air, which is why
 * the table is short - most of what a POSIX kernel exposes as a syscall is
 * here an IPC message to a service the caller holds an endpoint for.
 *
 * Calling convention per architecture:
 *   x86_64   syscall, number in rax, args in rdi rsi rdx r10 r8 r9, ret rax
 *   aarch64  svc #0,   number in x8,  args in x0..x5,               ret x0
 *   riscv64  ecall,    number in a7,  args in a0..a5,               ret a0
 */
#pragma once

#include <rk/types.h>

enum rk_syscall_nr {
	/* --- lifecycle ---------------------------------------------------- */
	SYS_EXIT = 0,
	SYS_THREAD_CREATE,
	SYS_THREAD_EXIT,
	SYS_THREAD_JOIN,
	SYS_YIELD,
	SYS_SLEEP_NS,
	SYS_TASK_CREATE,
	SYS_TASK_SPAWN,
	SYS_TASK_WAIT,
	SYS_GETPID,
	SYS_GETTID,

	/* --- capabilities -------------------------------------------------- */
	SYS_CAP_DERIVE = 32,
	SYS_CAP_GRANT,
	SYS_CAP_REVOKE,
	SYS_CAP_CLOSE,
	SYS_CAP_RIGHTS,
	SYS_CAP_INSPECT,

	/* --- memory --------------------------------------------------------- */
	SYS_MAP = 64,
	SYS_UNMAP,
	SYS_PROTECT,
	SYS_MEM_CREATE,
	SYS_BRK,

	/* --- ipc ------------------------------------------------------------ */
	SYS_ENDPOINT_CREATE = 96,
	SYS_CALL,
	SYS_SEND,
	SYS_RECV,
	SYS_REPLY,
	SYS_REPLY_RECV,
	SYS_CHANNEL_CREATE,
	SYS_CHANNEL_READ,
	SYS_CHANNEL_WRITE,
	SYS_NOTIFY_CREATE,
	SYS_NOTIFY_SIGNAL,
	SYS_NOTIFY_WAIT,

	/* --- files ---------------------------------------------------------- */
	SYS_OPEN = 128,
	SYS_CLOSE,
	SYS_READ,
	SYS_WRITE,
	SYS_PREAD,
	SYS_PWRITE,
	SYS_SEEK,
	SYS_STAT,
	SYS_READDIR,
	SYS_MKDIR,
	SYS_UNLINK,
	SYS_IOCTL,

	/* --- time and entropy ------------------------------------------------ */
	SYS_TIME_NS = 160,
	SYS_WALLCLOCK,
	SYS_RANDOM,

	/* --- runtime graph ---------------------------------------------------- */
	SYS_GRAPH_QUERY = 192,
	SYS_GRAPH_EXPORT,
	SYS_GRAPH_SNAPSHOT,
	SYS_GRAPH_EVENTS,
	SYS_GRAPH_DIFF,
	SYS_MEMFAB_GET,
	SYS_MEMFAB_PUT,

	/* --- ai ----------------------------------------------------------------- */
	SYS_MODEL_OPEN = 224,
	SYS_MODEL_INFO,
	SYS_TENSOR_CREATE,
	SYS_TENSOR_INFO,
	SYS_OP_EXEC,
	SYS_INFER_SUBMIT,
	SYS_INFER_WAIT,
	SYS_INFER_CANCEL,
	SYS_ACCEL_LIST,

	/* --- kaalka -------------------------------------------------------------- */
	SYS_SEAL_MAKE = 256,
	SYS_SEAL_VERIFY,
	SYS_ENVELOPE_SEAL,
	SYS_ENVELOPE_OPEN,

	/* --- she ----------------------------------------------------------------- */
	SYS_SHE_EVAL = 288,

	/* --- diagnostics --------------------------------------------------------- */
	SYS_LOG = 320,
	SYS_SYSINFO,
	SYS_DEBUG_PUTS,

	SYS_MAX = 384
};

/* Arguments arrive already copied out of registers; the handler is portable. */
struct rk_syscall_args {
	u64 nr;
	u64 a0, a1, a2, a3, a4, a5;
};

void rk_syscall_init(void);
s64  rk_syscall_dispatch(struct rk_syscall_args *a);
const char *rk_syscall_name(u64 nr);

/* Returned by SYS_SYSINFO. Deliberately flat and versioned so that a userspace
 * built against an older kernel still parses the prefix it knows. */
struct rk_sysinfo {
	u32 version;
	u32 size;
	char release[32];
	char arch[16];
	char platform[64];
	u64 uptime_ns;
	u64 mem_total, mem_free;
	u32 cpus;
	u32 tasks, threads;
	u64 graph_nodes, graph_events;
	u8  graph_root_digest[32];
	u64 kaalka_epoch;
};
