/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - SHE, the system language.
 *
 * SHE (https://github.com/ni-sh-a-char/SHE) is an English-like scripting
 * language whose defining idea is that a program starts with zero permissions
 * and must be granted each one explicitly. That is not a language feature in
 * RESENTMENT, it is the kernel security model, so SHE is not hosted on the
 * OS - it is the OS shell and policy language, and its sandbox is the
 * capability space in rk/cap.h.
 *
 * The practical consequence: when a SHE script tries to read a file without
 * the right, the kernel refuses at the capability check, the VM reports the
 * exact grant that was missing, and the same refusal would have happened to a
 * compiled binary. One mechanism, three kinds of caller.
 *
 * Deviations from the reference interpreter, all forced by running in ring 0:
 *
 *   - Numbers are 64-bit integers and Q32.32 fixed point, not IEEE doubles.
 *     The kernel avoids the FPU in interrupt context and the runtime graph
 *     promises bit-identical replay across architectures.
 *   - Execution is gas-metered. Every script has an instruction budget and a
 *     memory budget, so a runaway loop in a boot script cannot wedge the
 *     machine; it fails with a diagnostic instead.
 *   - Compilation is to a bytecode the VM executes in bounded steps, so a
 *     script is preemptible at instruction granularity like any other thread.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/cap.h>
#include <rk/kaalka.h>

/* ---------------------------------------------------------------- values */

enum she_valtype {
	SHE_NOTHING = 0,
	SHE_BOOL,
	SHE_NUM,        /* s64 */
	SHE_REAL,       /* Q32.32 fixed point */
	SHE_TEXT,
	SHE_LIST,
	SHE_MAP,
	SHE_RANGE,
	SHE_FUNCTION,
	SHE_NATIVE,
	SHE_CAP,        /* a capability handle, first class */
	SHE_TENSOR,
	SHE_VALTYPE_COUNT
};

struct she_obj;
struct she_vm;

struct she_value {
	u8 type;
	union {
		bool  b;
		s64   n;
		kq_t  r;
		struct she_obj *o;
		cap_handle_t    cap;
	} as;
};

#define SHE_NOTHING_V  ((struct she_value){ .type = SHE_NOTHING })
#define SHE_NUM_V(x)   ((struct she_value){ .type = SHE_NUM,  .as.n = (x) })
#define SHE_BOOL_V(x)  ((struct she_value){ .type = SHE_BOOL, .as.b = (x) })

const char *she_typename(enum she_valtype t);

/* Heap objects are refcounted and tracked per VM so a script cannot outlive
 * its own memory budget. */
struct she_obj {
	u8  type;
	u32 refcount;
	u32 len;
	struct she_vm *vm;
	struct list_head link;
	/* type-specific payload follows */
};

/* ---------------------------------------------------------------- bytecode */

enum she_op {
	OP_NOP = 0,
	OP_CONST, OP_NOTHING, OP_TRUE, OP_FALSE,
	OP_POP, OP_DUP, OP_SWAP,
	OP_GET_LOCAL, OP_SET_LOCAL,
	OP_GET_GLOBAL, OP_SET_GLOBAL, OP_DEF_GLOBAL,
	OP_GET_UPVAL, OP_SET_UPVAL, OP_CLOSE_UPVAL,
	OP_GET_INDEX, OP_SET_INDEX, OP_GET_FIELD, OP_SET_FIELD,
	OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_NEG,
	OP_EQ, OP_NEQ, OP_LT, OP_LE, OP_GT, OP_GE,
	OP_AND, OP_OR, OP_NOT,
	OP_CONCAT,
	OP_JUMP, OP_JUMP_IF_FALSE, OP_JUMP_IF_TRUE, OP_LOOP,
	OP_CALL, OP_RETURN, OP_CLOSURE,
	OP_MAKE_LIST, OP_MAKE_MAP, OP_MAKE_RANGE,
	OP_ITER_INIT, OP_ITER_NEXT,
	OP_MATCH_TEST,
	OP_PIPE,          /* the |> operator: feed lhs as first argument of rhs */
	OP_SAY, OP_ASK,
	OP_REQUIRE_CAP,   /* assert a capability right before a guarded region */
	OP_HALT,
	OP_COUNT
};

const char *she_opname(enum she_op op);

struct she_chunk {
	u8              *code;
	u32              len, cap;
	u32             *lines;        /* source line per instruction, for errors */
	struct she_value *consts;
	u32              nconsts, consts_cap;
};

/* ------------------------------------------------------------ permissions */

/* The grant set mirrors the SHE command line flags, and each one maps onto
 * concrete capability rights the kernel actually checks. */
#define SHE_ALLOW_READ    (1u << 0)
#define SHE_ALLOW_WRITE   (1u << 1)
#define SHE_ALLOW_NET     (1u << 2)
#define SHE_ALLOW_RUN     (1u << 3)
#define SHE_ALLOW_ENV     (1u << 4)
#define SHE_ALLOW_TIME    (1u << 5)
#define SHE_ALLOW_RANDOM  (1u << 6)
#define SHE_ALLOW_INFER   (1u << 7)
#define SHE_ALLOW_GRAPH   (1u << 8)
#define SHE_ALLOW_DEVICE  (1u << 9)
#define SHE_ALLOW_CAP     (1u << 10)  /* may mint and grant capabilities */
#define SHE_ALLOW_ALL     0x7ffu

u32         she_allow_parse(const char *name);
const char *she_allow_name(u32 bit);
const char *she_allow_flag(u32 bit);   /* e.g. "--allow-read" */

/* ---------------------------------------------------------------- the VM */

#define SHE_STACK_MAX  256
#define SHE_FRAMES_MAX 64

struct she_frame {
	struct she_obj *closure;
	u8             *ip;
	struct she_value *slots;
};

struct she_vm {
	struct she_value  stack[SHE_STACK_MAX];
	struct she_value *sp;
	struct she_frame  frames[SHE_FRAMES_MAX];
	u32               nframes;

	struct she_obj   *globals;      /* map */
	struct she_obj   *strings;      /* interning table */
	struct list_head  objects;      /* everything allocated by this VM */

	struct capspace  *caps;         /* the sandbox, enforced by the kernel */
	u32               allow;        /* SHE_ALLOW_* */

	u64               gas;          /* instructions remaining */
	u64               gas_limit;
	u64               mem_used, mem_limit;

	char              error[256];
	int               error_line;
	bool              failed;
	/* The compile error was "ran out of input", not "that is wrong". A REPL
	 * uses this to keep reading instead of rejecting a half-typed block. */
	bool              incomplete;

	rk_id_t           id;
	rk_id_t           graph_node;
	void             *io_ctx;
	void            (*write_out)(void *ctx, const char *s, size_t n);
	int             (*read_line)(void *ctx, char *buf, size_t n);
};

void she_vm_init(struct she_vm *vm, struct capspace *caps, u32 allow);
void she_vm_free(struct she_vm *vm);
void she_vm_set_limits(struct she_vm *vm, u64 gas, u64 mem_bytes);
void she_vm_set_io(struct she_vm *vm, void *ctx,
                   void (*out)(void *, const char *, size_t),
                   int (*in)(void *, char *, size_t));

/* Compile source into a callable function object. On failure the message in
 * vm->error is written for a human: it names the line, quotes the source, and
 * where a permission is missing it prints the exact flag that grants it. */
int she_compile(struct she_vm *vm, const char *source, const char *origin,
                struct she_obj **out_fn);

/* Run to completion, or until gas runs out (RK_EAGAIN, resumable). */
int she_run(struct she_vm *vm, struct she_obj *fn, struct she_value *result);
int she_resume(struct she_vm *vm, struct she_value *result);

/* One-shot helper used by the shell and by init. */
int she_eval(struct she_vm *vm, const char *source, const char *origin,
             struct she_value *result);

size_t she_value_repr(const struct she_value *v, char *buf, size_t n);

/* ------------------------------------------------------------- native API */

typedef int (*she_native_fn)(struct she_vm *vm, int argc, struct she_value *argv,
                             struct she_value *out);

struct she_native {
	const char   *name;
	she_native_fn fn;
	u8            min_args, max_args;
	u32           needs_allow;   /* refused unless the VM was granted this */
	const char   *doc;
};

int she_register_native(const struct she_native *n);
int she_register_natives(const struct she_native *arr, size_t count);
void she_stdlib_init(void);   /* registers math, text, list, map, time, ... */

/* Raise a permission error that tells the user exactly how to fix it. */
int she_deny(struct she_vm *vm, u32 need_allow, const char *what);
int she_error(struct she_vm *vm, const char *fmt, ...) __printf(2, 3);

/* ------------------------------------------------------------------ shell */

/* resh: the RESENTMENT shell. A SHE REPL with kernel builtins, which means
 * there is no separate shell grammar to learn and every command is a value. */
void resh_run(void) __noreturn;
int  resh_exec_line(struct she_vm *vm, const char *line);

struct she_stats {
	u64 vms_created, scripts_compiled, compile_errors;
	u64 instructions, gas_exhausted, denials;
	u64 objects_live, bytes_live;
};
void she_stats(struct she_stats *out);

int she_selftest(void);
