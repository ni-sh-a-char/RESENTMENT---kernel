/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - system call dispatch.
 *
 * Short by design. Almost every call takes a capability handle as its first
 * argument and fails with RK_EACCES if that handle does not carry the right,
 * so the table has no entries that grant authority out of nothing. What a
 * POSIX kernel exposes as a hundred syscalls is here an IPC message to a
 * service the caller holds an endpoint for.
 *
 * Every call is recorded in the runtime graph, which is what makes a session
 * replayable: the syscall stream plus the recorded timeline reconstructs the
 * run.
 */
#include <rk/syscall.h>
#include <rk/sched.h>
#include <rk/mm.h>
#include <rk/cap.h>
#include <rk/ipc.h>
#include <rk/vfs.h>
#include <rk/ai.h>
#include <rk/she.h>
#include <rk/graph.h>
#include <rk/time.h>
#include <rk/crypto.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/arch.h>
#include <rk/boot.h>
#include <rk/console.h>

#undef RK_SUBSYS
#define RK_SUBSYS "syscall"

#ifndef RK_VERSION
#define RK_VERSION "0.0.0-dev"
#endif

static u64 syscall_counts[SYS_MAX];

static const struct { u64 nr; const char *name; } syscall_names[] = {
	{ SYS_EXIT, "exit" }, { SYS_THREAD_CREATE, "thread_create" },
	{ SYS_THREAD_EXIT, "thread_exit" }, { SYS_THREAD_JOIN, "thread_join" },
	{ SYS_YIELD, "yield" }, { SYS_SLEEP_NS, "sleep_ns" },
	{ SYS_TASK_CREATE, "task_create" }, { SYS_TASK_SPAWN, "task_spawn" },
	{ SYS_TASK_WAIT, "task_wait" }, { SYS_GETPID, "getpid" },
	{ SYS_GETTID, "gettid" },
	{ SYS_CAP_DERIVE, "cap_derive" }, { SYS_CAP_GRANT, "cap_grant" },
	{ SYS_CAP_REVOKE, "cap_revoke" }, { SYS_CAP_CLOSE, "cap_close" },
	{ SYS_CAP_RIGHTS, "cap_rights" }, { SYS_CAP_INSPECT, "cap_inspect" },
	{ SYS_MAP, "map" }, { SYS_UNMAP, "unmap" }, { SYS_PROTECT, "protect" },
	{ SYS_MEM_CREATE, "mem_create" }, { SYS_BRK, "brk" },
	{ SYS_ENDPOINT_CREATE, "endpoint_create" }, { SYS_CALL, "call" },
	{ SYS_SEND, "send" }, { SYS_RECV, "recv" }, { SYS_REPLY, "reply" },
	{ SYS_REPLY_RECV, "reply_recv" },
	{ SYS_CHANNEL_CREATE, "channel_create" }, { SYS_CHANNEL_READ, "channel_read" },
	{ SYS_CHANNEL_WRITE, "channel_write" },
	{ SYS_NOTIFY_CREATE, "notify_create" }, { SYS_NOTIFY_SIGNAL, "notify_signal" },
	{ SYS_NOTIFY_WAIT, "notify_wait" },
	{ SYS_OPEN, "open" }, { SYS_CLOSE, "close" }, { SYS_READ, "read" },
	{ SYS_WRITE, "write" }, { SYS_PREAD, "pread" }, { SYS_PWRITE, "pwrite" },
	{ SYS_SEEK, "seek" }, { SYS_STAT, "stat" }, { SYS_READDIR, "readdir" },
	{ SYS_MKDIR, "mkdir" }, { SYS_UNLINK, "unlink" }, { SYS_IOCTL, "ioctl" },
	{ SYS_TIME_NS, "time_ns" }, { SYS_WALLCLOCK, "wallclock" },
	{ SYS_RANDOM, "random" },
	{ SYS_GRAPH_QUERY, "graph_query" }, { SYS_GRAPH_EXPORT, "graph_export" },
	{ SYS_GRAPH_SNAPSHOT, "graph_snapshot" }, { SYS_GRAPH_EVENTS, "graph_events" },
	{ SYS_GRAPH_DIFF, "graph_diff" },
	{ SYS_MEMFAB_GET, "memfab_get" }, { SYS_MEMFAB_PUT, "memfab_put" },
	{ SYS_MODEL_OPEN, "model_open" }, { SYS_MODEL_INFO, "model_info" },
	{ SYS_TENSOR_CREATE, "tensor_create" }, { SYS_TENSOR_INFO, "tensor_info" },
	{ SYS_OP_EXEC, "op_exec" }, { SYS_INFER_SUBMIT, "infer_submit" },
	{ SYS_INFER_WAIT, "infer_wait" }, { SYS_INFER_CANCEL, "infer_cancel" },
	{ SYS_ACCEL_LIST, "accel_list" },
	{ SYS_SEAL_MAKE, "seal_make" }, { SYS_SEAL_VERIFY, "seal_verify" },
	{ SYS_ENVELOPE_SEAL, "envelope_seal" }, { SYS_ENVELOPE_OPEN, "envelope_open" },
	{ SYS_SHE_EVAL, "she_eval" },
	{ SYS_LOG, "log" }, { SYS_SYSINFO, "sysinfo" },
	{ SYS_DEBUG_PUTS, "debug_puts" },
};

const char *rk_syscall_name(u64 nr)
{
	for (size_t i = 0; i < ARRAY_SIZE(syscall_names); i++)
		if (syscall_names[i].nr == nr)
			return syscall_names[i].name;
	return "unknown";
}

/* Handles are capabilities. Resolving one is the only way a syscall reaches a
 * kernel object, so the check cannot be skipped by a code path that forgot. */
static int resolve(cap_handle_t h, enum cap_type type, u32 rights, void **out)
{
	struct task *t = task_current();
	struct cap_object *o = NULL;

	if (!t || !t->caps)
		return RK_EACCES;
	int rc = cap_lookup(t->caps, h, type, rights, &o);
	if (rc != RK_OK)
		return rc;
	*out = o->ptr;
	return RK_OK;
}

/* -------------------------------------------------------------- handlers */

static s64 sys_file_read(struct rk_syscall_args *a)
{
	struct rk_file *f = NULL;
	int rc = resolve((cap_handle_t)a->a0, CAP_FILE, CAP_RIGHT_READ, (void **)&f);
	if (rc != RK_OK)
		return rc;

	size_t n = (size_t)a->a2;
	if (n > (1u << 20))
		n = 1u << 20;   /* bound a single transfer so one call cannot monopolise */

	void *tmp = kmalloc(n);
	if (!tmp)
		return RK_ENOMEM;

	ssize_t got = rk_file_read(f, tmp, n);
	if (got > 0 && copy_to_user((void *)(uintptr_t)a->a1, tmp, (size_t)got) != RK_OK)
		got = RK_EFAULT;
	kfree(tmp);
	return got;
}

static s64 sys_file_write(struct rk_syscall_args *a)
{
	struct rk_file *f = NULL;
	int rc = resolve((cap_handle_t)a->a0, CAP_FILE, CAP_RIGHT_WRITE, (void **)&f);
	if (rc != RK_OK)
		return rc;

	size_t n = (size_t)a->a2;
	if (n > (1u << 20))
		n = 1u << 20;

	void *tmp = kmalloc(n);
	if (!tmp)
		return RK_ENOMEM;
	if (copy_from_user(tmp, (const void *)(uintptr_t)a->a1, n) != RK_OK) {
		kfree(tmp);
		return RK_EFAULT;
	}
	ssize_t put = rk_file_write(f, tmp, n);
	kfree(tmp);
	return put;
}

static s64 sys_open(struct rk_syscall_args *a)
{
	char path[RK_PATH_MAX];
	int rc = strncpy_from_user(path, (const char *)(uintptr_t)a->a0, sizeof(path));
	if (rc < 0)
		return rc;

	struct rk_file *f = NULL;
	rc = rk_vfs_open(NULL, path, (u32)a->a1, (u32)a->a2, &f);
	if (rc != RK_OK)
		return rc;

	/* The returned handle is a capability whose rights mirror the open mode,
	 * so a file opened for reading cannot later be written through. */
	u32 rights = CAP_RIGHT_INSPECT;
	if (a->a1 & RK_O_READ)  rights |= CAP_RIGHT_READ;
	if (a->a1 & RK_O_WRITE) rights |= CAP_RIGHT_WRITE;

	struct task *t = task_current();
	struct cap_object *o = cap_object_create(CAP_FILE, f, "file", NULL);
	if (!o) {
		rk_file_put(f);
		return RK_ENOMEM;
	}
	cap_handle_t h = cap_install(t->caps, o, rights, 0, 3600);
	cap_object_put(o);
	return h == CAP_INVALID ? RK_ENFILE : h;
}

static s64 sys_graph_export(struct rk_syscall_args *a)
{
	size_t cap = (size_t)a->a2;
	if (cap > (1u << 20))
		cap = 1u << 20;

	char *buf = kmalloc(cap);
	if (!buf)
		return RK_ENOMEM;

	size_t n = rk_graph_export(rk_graph_root(), (enum graph_format)a->a0,
	                           (int)a->a3, buf, cap);
	if (n >= cap)
		n = cap - 1;
	int rc = copy_to_user((void *)(uintptr_t)a->a1, buf, n);
	kfree(buf);
	return rc == RK_OK ? (s64)n : rc;
}

static s64 sys_sysinfo(struct rk_syscall_args *a)
{
	struct rk_sysinfo info;
	struct pmm_stats p;
	struct sched_stats s;
	struct graph_stats g;

	memset(&info, 0, sizeof(info));
	pmm_stats(&p);
	sched_stats(&s);
	rk_graph_stats(&g);

	info.version = 1;
	info.size = sizeof(info);
	strlcpy(info.release, RK_VERSION, sizeof(info.release));
	strlcpy(info.arch, arch_name(), sizeof(info.arch));
	strlcpy(info.platform, rk_boot_info.platform, sizeof(info.platform));
	info.uptime_ns   = rk_time_ns();
	info.mem_total   = p.total_pages << RK_PAGE_SHIFT;
	info.mem_free    = p.free_pages << RK_PAGE_SHIFT;
	info.cpus        = arch_cpu_count();
	info.threads     = (u32)s.threads_live;
	info.graph_nodes = g.nodes;
	info.graph_events = g.events;
	info.kaalka_epoch = kaalka_epoch_now();
	const u8 *d = rk_graph_root_digest();
	if (d)
		memcpy(info.graph_root_digest, d, 32);

	return copy_to_user((void *)(uintptr_t)a->a0, &info, sizeof(info));
}

static s64 sys_she_eval(struct rk_syscall_args *a)
{
	size_t len = (size_t)a->a1;
	if (len > 65536)
		return RK_E2BIG;

	char *src = kmalloc(len + 1);
	if (!src)
		return RK_ENOMEM;
	if (copy_from_user(src, (const void *)(uintptr_t)a->a0, len) != RK_OK) {
		kfree(src);
		return RK_EFAULT;
	}
	src[len] = '\0';

	struct she_vm *vm = kzalloc(sizeof(*vm));
	if (!vm) {
		kfree(src);
		return RK_ENOMEM;
	}
	struct task *t = task_current();

	/* A script evaluated through the syscall gets exactly the permissions the
	 * caller asked for, intersected with nothing - the kernel does not add
	 * any. The capability space is the caller's, so a grant it does not hold
	 * is refused at the capability check regardless of the allow mask. */
	she_vm_init(vm, t ? t->caps : NULL, (u32)a->a2);
	extern void she_stdlib_bind(struct she_vm *);
	she_stdlib_bind(vm);
	she_vm_set_limits(vm, a->a3 ? a->a3 : 1000000, 1u << 20);

	struct she_value result;
	int rc = she_eval(vm, src, "syscall", &result);

	if (a->a4) {
		char buf[512];
		size_t n = rc == RK_OK ? she_value_repr(&result, buf, sizeof(buf))
		                       : (size_t)snprintf(buf, sizeof(buf), "%s", vm->error);
		if (n >= sizeof(buf))
			n = sizeof(buf) - 1;
		/* A bad output buffer has to be reported. Returning success with
		 * nothing written would leave the caller reading whatever was in
		 * its own memory and believing the kernel put it there. An
		 * evaluation error is kept in preference, being the more useful of
		 * the two. */
		int cr = copy_to_user((void *)(uintptr_t)a->a4, buf, n + 1);
		if (cr != RK_OK && rc == RK_OK)
			rc = cr;
	}

	she_vm_free(vm);
	kfree(vm);
	kfree(src);
	return rc;
}

static s64 sys_debug_puts(struct rk_syscall_args *a)
{
	char buf[256];
	int n = strncpy_from_user(buf, (const char *)(uintptr_t)a->a0, sizeof(buf));
	if (n < 0)
		return n;
	rk_console_write(buf, (size_t)n);
	return n;
}

/* ------------------------------------------------------------- dispatch */

s64 rk_syscall_dispatch(struct rk_syscall_args *a)
{
	if (a->nr >= SYS_MAX)
		return RK_ENOSYS;
	syscall_counts[a->nr]++;
	rk_graph_record(GEV_SYSCALL, 0, a->nr, a->a0, a->a1);

	switch (a->nr) {
	/* --- lifecycle --- */
	case SYS_EXIT:
		task_exit(task_current(), (int)a->a0);
		thread_exit((int)a->a0);
		return RK_OK;
	case SYS_THREAD_EXIT:
		thread_exit((int)a->a0);
		return RK_OK;
	case SYS_YIELD:
		sched_yield();
		return RK_OK;
	case SYS_SLEEP_NS:
		sched_sleep_ns(a->a0);
		return RK_OK;
	case SYS_GETPID: {
		struct task *t = task_current();
		return t ? (s64)t->pid : 0;
	}
	case SYS_GETTID: {
		struct thread *t = thread_current();
		return t ? (s64)t->tid : 0;
	}

	/* --- capabilities --- */
	case SYS_CAP_DERIVE: {
		struct task *t = task_current();
		return cap_derive(t->caps, (cap_handle_t)a->a0, (u32)a->a1, a->a2, a->a3);
	}
	case SYS_CAP_CLOSE: {
		struct task *t = task_current();
		return cap_close(t->caps, (cap_handle_t)a->a0);
	}
	case SYS_CAP_REVOKE: {
		struct task *t = task_current();
		return cap_revoke(t->caps, (cap_handle_t)a->a0);
	}
	case SYS_CAP_RIGHTS: {
		struct task *t = task_current();
		u32 rights = 0;
		int rc = cap_rights(t->caps, (cap_handle_t)a->a0, &rights);
		return rc == RK_OK ? (s64)rights : rc;
	}

	/* --- memory --- */
	case SYS_MAP: {
		struct task *t = task_current();
		vaddr_t out = 0;
		int rc = as_map_anon(t->as, (vaddr_t)a->a0, (size_t)a->a1, (u32)a->a2,
		                     "user", &out);
		return rc == RK_OK ? (s64)out : rc;
	}
	case SYS_UNMAP: {
		struct task *t = task_current();
		return as_unmap(t->as, (vaddr_t)a->a0, (size_t)a->a1);
	}
	case SYS_PROTECT: {
		struct task *t = task_current();
		return as_protect(t->as, (vaddr_t)a->a0, (size_t)a->a1, (u32)a->a2);
	}

	/* --- files --- */
	case SYS_OPEN:  return sys_open(a);
	case SYS_READ:  return sys_file_read(a);
	case SYS_WRITE: return sys_file_write(a);
	case SYS_CLOSE: {
		struct task *t = task_current();
		return cap_close(t->caps, (cap_handle_t)a->a0);
	}
	case SYS_SEEK: {
		struct rk_file *f = NULL;
		int rc = resolve((cap_handle_t)a->a0, CAP_FILE, CAP_RIGHT_INSPECT, (void **)&f);
		return rc == RK_OK ? rk_file_seek(f, (s64)a->a1, (int)a->a2) : rc;
	}

	/* --- ipc --- */
	case SYS_ENDPOINT_CREATE: {
		struct rk_endpoint *e = rk_endpoint_create("user");
		if (!e)
			return RK_ENOMEM;
		struct task *t = task_current();
		struct cap_object *o = cap_object_create(CAP_ENDPOINT, e, "endpoint", NULL);
		if (!o) {
			rk_endpoint_put(e);
			return RK_ENOMEM;
		}
		cap_handle_t h = cap_install(t->caps, o,
		                             CAP_RIGHT_SEND | CAP_RIGHT_RECV |
		                             CAP_RIGHT_GRANT | CAP_RIGHT_DERIVE, 0, 3600);
		cap_object_put(o);
		return h == CAP_INVALID ? RK_ENFILE : h;
	}
	case SYS_CALL: {
		struct rk_endpoint *e = NULL;
		int rc = resolve((cap_handle_t)a->a0, CAP_ENDPOINT, CAP_RIGHT_SEND, (void **)&e);
		if (rc != RK_OK)
			return rc;
		struct rk_message msg;
		if (copy_from_user(&msg, (const void *)(uintptr_t)a->a1, sizeof(msg)) != RK_OK)
			return RK_EFAULT;
		rc = rk_ipc_call(e, &msg, a->a2);
		if (rc == RK_OK)
			rc = copy_to_user((void *)(uintptr_t)a->a1, &msg, sizeof(msg));
		return rc;
	}

	/* --- time and entropy --- */
	case SYS_TIME_NS:   return (s64)rk_time_ns();
	case SYS_WALLCLOCK: return rk_unix_time();
	case SYS_RANDOM: {
		size_t n = (size_t)a->a1;
		if (n > 1024)
			n = 1024;   /* one syscall, one page-sized bite */
		u8 buf[1024];
		rk_random_bytes(buf, n);
		int rc = copy_to_user((void *)(uintptr_t)a->a0, buf, n);
		rk_secure_zero(buf, n);
		return rc == RK_OK ? (s64)n : rc;
	}

	/* --- runtime graph --- */
	case SYS_GRAPH_EXPORT: return sys_graph_export(a);
	case SYS_GRAPH_EVENTS: {
		size_t max = (size_t)a->a1;
		if (max > 256)
			max = 256;
		struct graph_event *ev = kcalloc(max, sizeof(*ev));
		if (!ev)
			return RK_ENOMEM;
		u64 cursor = a->a2;
		size_t n = rk_graph_events_read(ev, max, &cursor);
		int rc = copy_to_user((void *)(uintptr_t)a->a0, ev, n * sizeof(*ev));
		kfree(ev);
		return rc == RK_OK ? (s64)n : rc;
	}
	case SYS_MEMFAB_PUT: {
		size_t n = (size_t)a->a2;
		if (n > 65536)
			return RK_E2BIG;
		char key[128];
		if (strncpy_from_user(key, (const char *)(uintptr_t)a->a0, sizeof(key)) < 0)
			return RK_EFAULT;
		void *val = kmalloc(n);
		if (!val)
			return RK_ENOMEM;
		if (copy_from_user(val, (const void *)(uintptr_t)a->a1, n) != RK_OK) {
			kfree(val);
			return RK_EFAULT;
		}
		int rc = rk_memfab_put_str(key, val, n);
		kfree(val);
		return rc;
	}
	case SYS_MEMFAB_GET: {
		char key[128];
		if (strncpy_from_user(key, (const char *)(uintptr_t)a->a0, sizeof(key)) < 0)
			return RK_EFAULT;
		size_t cap = (size_t)a->a2;
		if (cap > 65536)
			cap = 65536;
		void *val = kmalloc(cap);
		if (!val)
			return RK_ENOMEM;
		size_t n = rk_memfab_get_str(key, val, cap);
		int rc = n ? copy_to_user((void *)(uintptr_t)a->a1, val, n) : RK_OK;
		kfree(val);
		return rc == RK_OK ? (s64)n : rc;
	}

	/* --- kaalka --- */
	case SYS_SEAL_MAKE: {
		size_t n = (size_t)a->a1;
		if (n > 1024)
			return RK_E2BIG;
		u8 buf[1024];
		if (copy_from_user(buf, (const void *)(uintptr_t)a->a0, n) != RK_OK)
			return RK_EFAULT;
		struct kaalka_seal seal;
		struct task *t = task_current();
		kaalka_seal_make(&seal, t ? t->pid : 0, a->a3, buf, n, a->a2);
		return copy_to_user((void *)(uintptr_t)a->a4, &seal, sizeof(seal));
	}

	/* --- she --- */
	case SYS_SHE_EVAL: return sys_she_eval(a);

	/* --- diagnostics --- */
	case SYS_SYSINFO:    return sys_sysinfo(a);
	case SYS_DEBUG_PUTS: return sys_debug_puts(a);
	case SYS_LOG: {
		char buf[256];
		int n = strncpy_from_user(buf, (const char *)(uintptr_t)a->a1, sizeof(buf));
		if (n < 0)
			return n;
		rk_log((enum rk_loglevel)a->a0, "user", "%s", buf);
		return RK_OK;
	}

	default:
		pr_debug("unimplemented syscall %llu (%s)",
		         (unsigned long long)a->nr, rk_syscall_name(a->nr));
		return RK_ENOSYS;
	}
}

u64 rk_syscall_count(u64 nr)
{
	return nr < SYS_MAX ? syscall_counts[nr] : 0;
}

void rk_syscall_init(void)
{
	memset(syscall_counts, 0, sizeof(syscall_counts));
	pr_info("system call interface ready, %u slots", SYS_MAX);
}
