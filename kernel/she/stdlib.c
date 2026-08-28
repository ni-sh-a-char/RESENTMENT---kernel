/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the SHE standard library.
 *
 * Every builtin that reaches outside the VM declares which grant it needs, and
 * the VM refuses it with a message naming that grant. Pure functions - text,
 * list and arithmetic helpers - need nothing, so a script with zero
 * permissions can still compute.
 *
 * The kernel-specific half of this library is the interesting part: a SHE
 * script can read the runtime graph, take a Kaalka-sealed snapshot, list
 * capabilities, run inference and inspect the scheduler, all through the same
 * permission model that governs reading a file.
 */
#include "she_internal.h"
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/time.h>
#include <rk/crypto.h>
#include <rk/graph.h>
#include <rk/sched.h>
#include <rk/vfs.h>
#include <rk/ai.h>
#include <rk/arch.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "she"

/* --------------------------------------------------------- permissions */

static const struct {
	u32 bit;
	const char *name;
	const char *flag;
} allow_table[] = {
	{ SHE_ALLOW_READ,   "permission to read files",        "--allow-read" },
	{ SHE_ALLOW_WRITE,  "permission to write files",       "--allow-write" },
	{ SHE_ALLOW_NET,    "permission to use the network",   "--allow-net" },
	{ SHE_ALLOW_RUN,    "permission to run programs",      "--allow-run" },
	{ SHE_ALLOW_ENV,    "permission to read the environment", "--allow-env" },
	{ SHE_ALLOW_TIME,   "permission to read the clock",    "--allow-time" },
	{ SHE_ALLOW_RANDOM, "permission to use randomness",    "--allow-random" },
	{ SHE_ALLOW_INFER,  "permission to run models",        "--allow-infer" },
	{ SHE_ALLOW_GRAPH,  "permission to read the runtime graph", "--allow-graph" },
	{ SHE_ALLOW_DEVICE, "permission to touch devices",     "--allow-device" },
	{ SHE_ALLOW_CAP,    "permission to mint capabilities", "--allow-cap" },
};

u32 she_allow_parse(const char *name)
{
	if (strcmp(name, "all") == 0)
		return SHE_ALLOW_ALL;
	for (size_t i = 0; i < ARRAY_SIZE(allow_table); i++) {
		const char *flag = allow_table[i].flag + 8;   /* skip "--allow-" */
		if (strcmp(name, flag) == 0)
			return allow_table[i].bit;
	}
	return 0;
}

const char *she_allow_name(u32 bit)
{
	for (size_t i = 0; i < ARRAY_SIZE(allow_table); i++)
		if (allow_table[i].bit == bit)
			return allow_table[i].name;
	return "that permission";
}

const char *she_allow_flag(u32 bit)
{
	for (size_t i = 0; i < ARRAY_SIZE(allow_table); i++)
		if (allow_table[i].bit == bit)
			return allow_table[i].flag;
	return "--allow-all";
}

/* --------------------------------------------------------------- natives */

#define ARG(i) (i < argc ? argv[i] : SHE_NOTHING_V)

static s64 as_num(struct she_value v)
{
	if (v.type == SHE_NUM)  return v.as.n;
	if (v.type == SHE_REAL) return kq_to_int(v.as.r);
	if (v.type == SHE_BOOL) return v.as.b ? 1 : 0;
	return 0;
}

/* -- text ---------------------------------------------------------------- */

static int nat_length(struct she_vm *vm, int argc, struct she_value *argv,
                      struct she_value *out)
{
	(void)vm;
	struct she_value v = ARG(0);
	*out = SHE_NUM_V(v.as.o && v.type >= SHE_TEXT ? v.as.o->len : 0);
	return RK_OK;
}

static int nat_upper(struct she_vm *vm, int argc, struct she_value *argv,
                     struct she_value *out)
{
	struct she_value v = ARG(0);
	if (v.type != SHE_TEXT)
		return she_error(vm, "upper needs text, not %s", she_typename(v.type));

	char buf[256];
	const char *s = she_string_chars(v.as.o);
	size_t n = strnlen(s, sizeof(buf) - 1);
	for (size_t i = 0; i < n; i++)
		buf[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
	*out = she_obj_value(SHE_TEXT, she_string_new(vm, buf, n));
	return RK_OK;
}

static int nat_lower(struct she_vm *vm, int argc, struct she_value *argv,
                     struct she_value *out)
{
	struct she_value v = ARG(0);
	if (v.type != SHE_TEXT)
		return she_error(vm, "lower needs text, not %s", she_typename(v.type));

	char buf[256];
	const char *s = she_string_chars(v.as.o);
	size_t n = strnlen(s, sizeof(buf) - 1);
	for (size_t i = 0; i < n; i++)
		buf[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
	*out = she_obj_value(SHE_TEXT, she_string_new(vm, buf, n));
	return RK_OK;
}

static int nat_text(struct she_vm *vm, int argc, struct she_value *argv,
                    struct she_value *out)
{
	char buf[512];
	struct she_value v = ARG(0);
	size_t n = she_value_repr(&v, buf, sizeof(buf));
	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	*out = she_obj_value(SHE_TEXT, she_string_new(vm, buf, n));
	return RK_OK;
}

static int nat_number(struct she_vm *vm, int argc, struct she_value *argv,
                      struct she_value *out)
{
	(void)vm;
	struct she_value v = ARG(0);
	if (v.type == SHE_TEXT)
		*out = SHE_NUM_V(rk_strtos64(she_string_chars(v.as.o), NULL, 10));
	else
		*out = SHE_NUM_V(as_num(v));
	return RK_OK;
}

static int nat_contains(struct she_vm *vm, int argc, struct she_value *argv,
                        struct she_value *out)
{
	(void)vm;
	struct she_value hay = ARG(0), needle = ARG(1);
	if (hay.type == SHE_TEXT && needle.type == SHE_TEXT) {
		*out = SHE_BOOL_V(strstr(she_string_chars(hay.as.o),
		                         she_string_chars(needle.as.o)) != NULL);
		return RK_OK;
	}
	if (hay.type == SHE_LIST) {
		for (u32 i = 0; i < hay.as.o->len; i++) {
			if (she_equal(she_list_get(hay.as.o, i), needle)) {
				*out = SHE_BOOL_V(true);
				return RK_OK;
			}
		}
	}
	*out = SHE_BOOL_V(false);
	return RK_OK;
}

/* -- lists --------------------------------------------------------------- */

static int nat_push(struct she_vm *vm, int argc, struct she_value *argv,
                    struct she_value *out)
{
	struct she_value l = ARG(0);
	if (l.type != SHE_LIST)
		return she_error(vm, "push needs a list, not %s", she_typename(l.type));
	she_list_push(vm, l.as.o, ARG(1));
	*out = l;
	return RK_OK;
}

static int nat_sum(struct she_vm *vm, int argc, struct she_value *argv,
                   struct she_value *out)
{
	struct she_value l = ARG(0);
	if (l.type == SHE_RANGE) {
		s64 total = 0;
		for (u32 i = 0; i < l.as.o->len; i++)
			total += she_range_at(l.as.o, i);
		*out = SHE_NUM_V(total);
		return RK_OK;
	}
	if (l.type != SHE_LIST)
		return she_error(vm, "sum needs a list, not %s", she_typename(l.type));

	s64 whole = 0;
	kq_t real = 0;
	bool is_real = false;
	for (u32 i = 0; i < l.as.o->len; i++) {
		struct she_value v = she_list_get(l.as.o, i);
		if (v.type == SHE_REAL) {
			is_real = true;
			real += v.as.r;
		} else {
			whole += as_num(v);
		}
	}
	if (is_real) {
		out->type = SHE_REAL;
		out->as.r = real + kq_from_int(whole);
	} else {
		*out = SHE_NUM_V(whole);
	}
	return RK_OK;
}

/* map and filter take a function value, so they need the VM to call back into
 * itself. Doing that from a native means re-entering the interpreter, which is
 * why these build the result eagerly rather than lazily: a lazy sequence would
 * need a continuation the VM does not have. */
static int call_one(struct she_vm *vm, struct she_value fn, struct she_value arg,
                    struct she_value *out);

static int nat_map(struct she_vm *vm, int argc, struct she_value *argv,
                   struct she_value *out)
{
	struct she_value l = ARG(0), f = ARG(1);
	if (l.type != SHE_LIST && l.type != SHE_RANGE)
		return she_error(vm, "map needs a list, not %s", she_typename(l.type));

	struct she_obj *res = she_list_new(vm, l.as.o->len);
	if (!res)
		return RK_ENOMEM;

	for (u32 i = 0; i < l.as.o->len; i++) {
		struct she_value item = l.type == SHE_RANGE
		                      ? SHE_NUM_V(she_range_at(l.as.o, i))
		                      : she_list_get(l.as.o, i);
		struct she_value mapped;
		int rc = call_one(vm, f, item, &mapped);
		if (rc != RK_OK)
			return rc;
		she_list_push(vm, res, mapped);
	}
	*out = she_obj_value(SHE_LIST, res);
	return RK_OK;
}

static int nat_filter(struct she_vm *vm, int argc, struct she_value *argv,
                      struct she_value *out)
{
	struct she_value l = ARG(0), f = ARG(1);
	if (l.type != SHE_LIST && l.type != SHE_RANGE)
		return she_error(vm, "filter needs a list, not %s", she_typename(l.type));

	struct she_obj *res = she_list_new(vm, 0);
	if (!res)
		return RK_ENOMEM;

	for (u32 i = 0; i < l.as.o->len; i++) {
		struct she_value item = l.type == SHE_RANGE
		                      ? SHE_NUM_V(she_range_at(l.as.o, i))
		                      : she_list_get(l.as.o, i);
		struct she_value keep;
		int rc = call_one(vm, f, item, &keep);
		if (rc != RK_OK)
			return rc;
		if (she_truthy(keep))
			she_list_push(vm, res, item);
	}
	*out = she_obj_value(SHE_LIST, res);
	return RK_OK;
}

/* -- maths --------------------------------------------------------------- */

static int nat_abs(struct she_vm *vm, int argc, struct she_value *argv,
                   struct she_value *out)
{
	(void)vm;
	struct she_value v = ARG(0);
	if (v.type == SHE_REAL) {
		out->type = SHE_REAL;
		out->as.r = v.as.r < 0 ? -v.as.r : v.as.r;
	} else {
		s64 n = as_num(v);
		*out = SHE_NUM_V(n < 0 ? -n : n);
	}
	return RK_OK;
}

static int nat_min(struct she_vm *vm, int argc, struct she_value *argv,
                   struct she_value *out)
{
	(void)vm;
	s64 a = as_num(ARG(0)), b = as_num(ARG(1));
	*out = SHE_NUM_V(a < b ? a : b);
	return RK_OK;
}

static int nat_max(struct she_vm *vm, int argc, struct she_value *argv,
                   struct she_value *out)
{
	(void)vm;
	s64 a = as_num(ARG(0)), b = as_num(ARG(1));
	*out = SHE_NUM_V(a > b ? a : b);
	return RK_OK;
}

static int nat_random(struct she_vm *vm, int argc, struct she_value *argv,
                      struct she_value *out)
{
	(void)vm;
	s64 lo = argc > 0 ? as_num(ARG(0)) : 0;
	s64 hi = argc > 1 ? as_num(ARG(1)) : 100;
	if (hi <= lo)
		hi = lo + 1;
	*out = SHE_NUM_V(lo + (s64)(rk_random_u64() % (u64)(hi - lo)));
	return RK_OK;
}

/* -- time ---------------------------------------------------------------- */

static int nat_now(struct she_vm *vm, int argc, struct she_value *argv,
                   struct she_value *out)
{
	(void)vm; (void)argc; (void)argv;
	*out = SHE_NUM_V(rk_unix_time());
	return RK_OK;
}

static int nat_uptime(struct she_vm *vm, int argc, struct she_value *argv,
                      struct she_value *out)
{
	(void)vm; (void)argc; (void)argv;
	*out = SHE_NUM_V((s64)rk_uptime_sec());
	return RK_OK;
}

static int nat_sleep(struct she_vm *vm, int argc, struct she_value *argv,
                     struct she_value *out)
{
	(void)vm;
	s64 ms = as_num(ARG(0));
	/* Bounded: a script must not be able to park a kernel thread forever. */
	if (ms > 60000)
		ms = 60000;
	if (ms > 0)
		sched_sleep_ms((u64)ms);
	*out = SHE_NOTHING_V;
	return RK_OK;
}

/* -- files --------------------------------------------------------------- */

static int nat_read_file(struct she_vm *vm, int argc, struct she_value *argv,
                         struct she_value *out)
{
	struct she_value p = ARG(0);
	if (p.type != SHE_TEXT)
		return she_error(vm, "read needs a path as text");

	void *buf = NULL;
	size_t len = 0;
	int rc = rk_vfs_read_file(she_string_chars(p.as.o), &buf, &len);
	if (rc != RK_OK)
		return she_error(vm, "cannot read %s: %s",
		                 she_string_chars(p.as.o), rk_strerror(rc));

	*out = she_obj_value(SHE_TEXT, she_string_new(vm, buf, len));
	kfree(buf);
	return RK_OK;
}

static int nat_write_file(struct she_vm *vm, int argc, struct she_value *argv,
                          struct she_value *out)
{
	struct she_value p = ARG(0), d = ARG(1);
	if (p.type != SHE_TEXT || d.type != SHE_TEXT)
		return she_error(vm, "write needs a path and text");

	int rc = rk_vfs_write_file(she_string_chars(p.as.o),
	                           she_string_chars(d.as.o), d.as.o->len);
	if (rc != RK_OK)
		return she_error(vm, "cannot write %s: %s",
		                 she_string_chars(p.as.o), rk_strerror(rc));
	*out = SHE_BOOL_V(true);
	return RK_OK;
}

/* -- the runtime graph --------------------------------------------------- */

static int nat_graph_digest(struct she_vm *vm, int argc, struct she_value *argv,
                            struct she_value *out)
{
	(void)argc; (void)argv;
	const u8 *d = rk_graph_root_digest();
	char hex[65];
	rk_hex_encode(hex, sizeof(hex), d, RK_GRAPH_DIGEST_SIZE);
	*out = she_text(vm, hex);
	return RK_OK;
}

static int nat_graph_read(struct she_vm *vm, int argc, struct she_value *argv,
                          struct she_value *out)
{
	struct she_value f = ARG(0);
	const char *what = f.type == SHE_TEXT ? she_string_chars(f.as.o) : "tree";

	enum graph_format fmt = GRAPH_FMT_TEXT;
	if (strcmp(what, "json") == 0)  fmt = GRAPH_FMT_JSON;
	else if (strcmp(what, "canon") == 0) fmt = GRAPH_FMT_CANON;
	else if (strcmp(what, "dot") == 0)   fmt = GRAPH_FMT_DOT;

	size_t cap = 32768;
	char *buf = kmalloc(cap);
	if (!buf)
		return RK_ENOMEM;
	size_t n = rk_graph_export(rk_graph_root(), fmt, -1, buf, cap);
	if (n >= cap)
		n = cap - 1;
	*out = she_obj_value(SHE_TEXT, she_string_new(vm, buf, n));
	kfree(buf);
	return RK_OK;
}

static int nat_snapshot(struct she_vm *vm, int argc, struct she_value *argv,
                        struct she_value *out)
{
	(void)argc; (void)argv;
	size_t cap = 65536;
	char *buf = kmalloc(cap);
	if (!buf)
		return RK_ENOMEM;

	struct graph_snapshot s;
	int rc = rk_graph_snapshot(&s, buf, cap);
	kfree(buf);
	if (rc != RK_OK)
		return she_error(vm, "snapshot failed: %s", rk_strerror(rc));

	char hex[65];
	rk_hex_encode(hex, sizeof(hex), s.root_digest, RK_GRAPH_DIGEST_SIZE);

	struct she_obj *m = she_map_new(vm);
	she_map_set(vm, m, she_string_new(vm, "digest", 6), she_text(vm, hex));
	she_map_set(vm, m, she_string_new(vm, "nodes", 5), SHE_NUM_V(s.node_count));
	she_map_set(vm, m, she_string_new(vm, "bytes", 5), SHE_NUM_V(s.byte_len));
	she_map_set(vm, m, she_string_new(vm, "at", 2), SHE_NUM_V(s.unix_sec));
	*out = she_obj_value(SHE_MAP, m);
	return RK_OK;
}

/* -- memory fabric ------------------------------------------------------- */

static int nat_remember(struct she_vm *vm, int argc, struct she_value *argv,
                        struct she_value *out)
{
	struct she_value k = ARG(0), v = ARG(1);
	if (k.type != SHE_TEXT)
		return she_error(vm, "remember needs a name as text");

	char buf[512];
	size_t n = she_value_repr(&v, buf, sizeof(buf));
	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	int rc = rk_memfab_put_str(she_string_chars(k.as.o), buf, n);
	*out = SHE_BOOL_V(rc == RK_OK);
	return RK_OK;
}

static int nat_recall(struct she_vm *vm, int argc, struct she_value *argv,
                      struct she_value *out)
{
	struct she_value k = ARG(0);
	if (k.type != SHE_TEXT)
		return she_error(vm, "recall needs a name as text");

	char buf[512];
	size_t n = rk_memfab_get_str(she_string_chars(k.as.o), buf, sizeof(buf) - 1);
	if (!n) {
		*out = SHE_NOTHING_V;
		return RK_OK;
	}
	*out = she_obj_value(SHE_TEXT, she_string_new(vm, buf, n));
	return RK_OK;
}

/* -- kaalka -------------------------------------------------------------- */

static int nat_seal(struct she_vm *vm, int argc, struct she_value *argv,
                    struct she_value *out)
{
	struct she_value v = ARG(0);
	s64 lifetime = argc > 1 ? as_num(ARG(1)) : 60;

	char buf[512];
	size_t n = she_value_repr(&v, buf, sizeof(buf));
	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;

	struct kaalka_seal seal;
	kaalka_seal_make(&seal, vm->id, rk_time_ns(), buf, n,
	                 (u64)(lifetime > 0 ? lifetime : 60));

	char hex[65];
	rk_hex_encode(hex, sizeof(hex), seal.mac, 32);

	struct she_obj *m = she_map_new(vm);
	she_map_set(vm, m, she_string_new(vm, "mac", 3), she_text(vm, hex));
	she_map_set(vm, m, she_string_new(vm, "epoch", 5), SHE_NUM_V((s64)seal.epoch));
	she_map_set(vm, m, she_string_new(vm, "until", 5), SHE_NUM_V(seal.not_after));
	*out = she_obj_value(SHE_MAP, m);
	return RK_OK;
}

static int nat_clock_angles(struct she_vm *vm, int argc, struct she_value *argv,
                            struct she_value *out)
{
	(void)argc; (void)argv;
	struct kaalka_time kt;
	struct kaalka_angles ang;
	kaalka_time_now(&kt);
	kaalka_angles(&kt, &ang);

	struct she_obj *m = she_map_new(vm);
	struct she_value hm = { .type = SHE_REAL }, ms = { .type = SHE_REAL },
	                 hs = { .type = SHE_REAL };
	hm.as.r = ang.hour_min;
	ms.as.r = ang.min_sec;
	hs.as.r = ang.hour_sec;
	she_map_set(vm, m, she_string_new(vm, "hour_minute", 11), hm);
	she_map_set(vm, m, she_string_new(vm, "minute_second", 13), ms);
	she_map_set(vm, m, she_string_new(vm, "hour_second", 11), hs);
	*out = she_obj_value(SHE_MAP, m);
	return RK_OK;
}

/* -- system -------------------------------------------------------------- */

static int nat_sysinfo(struct she_vm *vm, int argc, struct she_value *argv,
                       struct she_value *out)
{
	(void)argc; (void)argv;
	struct pmm_stats p;
	struct sched_stats s;
	pmm_stats(&p);
	sched_stats(&s);

	struct she_obj *m = she_map_new(vm);
	she_map_set(vm, m, she_string_new(vm, "arch", 4), she_text(vm, arch_name()));
	she_map_set(vm, m, she_string_new(vm, "cpu", 3), she_text(vm, arch_cpu_model()));
	she_map_set(vm, m, she_string_new(vm, "cpus", 4), SHE_NUM_V(arch_cpu_count()));
	she_map_set(vm, m, she_string_new(vm, "memory_free", 11),
	            SHE_NUM_V((s64)(p.free_pages << RK_PAGE_SHIFT)));
	she_map_set(vm, m, she_string_new(vm, "memory_total", 12),
	            SHE_NUM_V((s64)(p.total_pages << RK_PAGE_SHIFT)));
	she_map_set(vm, m, she_string_new(vm, "threads", 7),
	            SHE_NUM_V((s64)s.threads_live));
	she_map_set(vm, m, she_string_new(vm, "uptime", 6),
	            SHE_NUM_V((s64)rk_uptime_sec()));
	*out = she_obj_value(SHE_MAP, m);
	return RK_OK;
}

static int nat_models(struct she_vm *vm, int argc, struct she_value *argv,
                      struct she_value *out)
{
	(void)argc; (void)argv;
	struct rk_model *list[16];
	size_t n = rk_model_list(list, ARRAY_SIZE(list));

	struct she_obj *l = she_list_new(vm, (u32)n);
	for (size_t i = 0; i < n; i++)
		she_list_push(vm, l, she_text(vm, list[i]->name));
	*out = she_obj_value(SHE_LIST, l);
	return RK_OK;
}

/* ---------------------------------------------------------------- table */

static const struct she_native natives[] = {
	/* pure: no permission required */
	{ "length",   nat_length,   1, 1, 0, "how many items or characters" },
	{ "count",    nat_length,   1, 1, 0, "alias for length" },
	{ "upper",    nat_upper,    1, 1, 0, "text in capitals" },
	{ "lower",    nat_lower,    1, 1, 0, "text in lower case" },
	{ "text",     nat_text,     1, 1, 0, "anything as text" },
	{ "number",   nat_number,   1, 1, 0, "text as a number" },
	{ "contains", nat_contains, 2, 2, 0, "does it hold this" },
	{ "push",     nat_push,     2, 2, 0, "add to the end of a list" },
	{ "sum",      nat_sum,      1, 1, 0, "add up a list or range" },
	{ "map",      nat_map,      2, 2, 0, "apply a function to every item" },
	{ "filter",   nat_filter,   2, 2, 0, "keep the items a test accepts" },
	{ "abs",      nat_abs,      1, 1, 0, "size without a sign" },
	{ "min",      nat_min,      2, 2, 0, "the smaller of two" },
	{ "max",      nat_max,      2, 2, 0, "the larger of two" },

	/* guarded */
	{ "random",   nat_random,   0, 2, SHE_ALLOW_RANDOM, "a random whole number" },
	{ "now",      nat_now,      0, 0, SHE_ALLOW_TIME,   "seconds since 1970" },
	{ "uptime",   nat_uptime,   0, 0, SHE_ALLOW_TIME,   "seconds since boot" },
	{ "sleep",    nat_sleep,    1, 1, SHE_ALLOW_TIME,   "wait, in milliseconds" },
	{ "read",     nat_read_file,  1, 1, SHE_ALLOW_READ,  "read a whole file" },
	{ "write",    nat_write_file, 2, 2, SHE_ALLOW_WRITE, "write a whole file" },
	{ "graph",    nat_graph_read, 0, 1, SHE_ALLOW_GRAPH, "the runtime graph as text" },
	{ "digest",   nat_graph_digest, 0, 0, SHE_ALLOW_GRAPH, "the system merkle root" },
	{ "snapshot", nat_snapshot, 0, 0, SHE_ALLOW_GRAPH,  "a sealed state snapshot" },
	{ "remember", nat_remember, 2, 2, SHE_ALLOW_WRITE,  "store in the memory fabric" },
	{ "recall",   nat_recall,   1, 1, SHE_ALLOW_READ,   "read from the memory fabric" },
	{ "seal",     nat_seal,     1, 2, SHE_ALLOW_CAP,    "make a Kaalka seal" },
	{ "angles",   nat_clock_angles, 0, 0, SHE_ALLOW_TIME, "the live Kaalka clock angles" },
	{ "system",   nat_sysinfo,  0, 0, SHE_ALLOW_GRAPH,  "facts about this machine" },
	{ "models",   nat_models,   0, 0, SHE_ALLOW_INFER,  "loaded models" },
};

/* The registry is global, but each VM gets its own native objects so that the
 * permission check is per-VM. */
static const struct she_native *registry[64];
static size_t registry_count;

int she_register_native(const struct she_native *n)
{
	if (registry_count >= ARRAY_SIZE(registry))
		return RK_ENOSPC;
	registry[registry_count++] = n;
	return RK_OK;
}

int she_register_natives(const struct she_native *arr, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int r = she_register_native(&arr[i]);
		if (r != RK_OK)
			return r;
	}
	return RK_OK;
}

void she_stdlib_init(void)
{
	registry_count = 0;
	she_register_natives(natives, ARRAY_SIZE(natives));
	pr_info("SHE standard library: %u builtins", (unsigned)registry_count);
}

/* Called once per VM, after she_vm_init, to bind the registry into globals. */
void she_stdlib_bind(struct she_vm *vm)
{
	for (size_t i = 0; i < registry_count; i++) {
		struct she_obj *o = she_native_new(vm, registry[i]);
		if (!o)
			return;
		she_global_set(vm, registry[i]->name, she_obj_value(SHE_NATIVE, o));
	}
}

/* Callbacks re-enter the interpreter on the current stack rather than copying
 * it, so a callback costs one frame and nothing else. Frame and gas limits
 * still apply, so a callback that recurses fails cleanly. */
static int call_one(struct she_vm *vm, struct she_value fn, struct she_value arg,
                    struct she_value *out)
{
	if (fn.type != SHE_FUNCTION && fn.type != SHE_NATIVE)
		return she_error(vm, "map and filter need a function, not %s",
		                 she_typename(fn.type));
	return she_call_now(vm, fn, &arg, 1, out);
}
