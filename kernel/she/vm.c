/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the SHE virtual machine.
 *
 * A stack machine with a gas budget. Every instruction costs one unit; when
 * the budget runs out the VM stops and returns RK_EAGAIN with its state
 * intact, so the caller can decide whether to give it more or abandon it.
 * That is what makes it safe to run an arbitrary script inside the kernel:
 * the worst a runaway loop can do is use its allowance and stop.
 *
 * The other half of the safety story is that every operation reaching outside
 * the VM goes through a capability check, and a refusal names the exact grant
 * that was missing.
 */
#include "she_internal.h"
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/console.h>
#include <rk/printf.h>
#include <rk/graph.h>
#include <rk/sched.h>

#undef RK_SUBSYS
#define RK_SUBSYS "she"

static struct she_stats sstats;
static rk_id_t next_vm_id = 1;

static const char *const opnames[OP_COUNT] = {
	"nop", "const", "nothing", "true", "false", "pop", "dup", "swap",
	"get_local", "set_local", "get_global", "set_global", "def_global",
	"get_upval", "set_upval", "close_upval",
	"get_index", "set_index", "get_field", "set_field",
	"add", "sub", "mul", "div", "mod", "pow", "neg",
	"eq", "neq", "lt", "le", "gt", "ge", "and", "or", "not", "concat",
	"jump", "jump_if_false", "jump_if_true", "loop",
	"call", "return", "closure",
	"make_list", "make_map", "make_range", "iter_init", "iter_next",
	"match_test", "pipe", "say", "ask", "require_cap", "halt"
};

const char *she_opname(enum she_op op)
{
	return (unsigned)op < OP_COUNT ? opnames[op] : "?";
}

/* ------------------------------------------------------------ diagnostics */

int she_error(struct she_vm *vm, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(vm->error, sizeof(vm->error), fmt, ap);
	va_end(ap);
	vm->failed = true;
	return RK_EINVAL;
}

/* The refusal that makes the capability model teachable: it says what was
 * blocked and prints the flag that would have allowed it. */
int she_deny(struct she_vm *vm, u32 need_allow, const char *what)
{
	sstats.denials++;
	rk_graph_record(GEV_CAP_CHECK, vm->graph_node, need_allow, 0, 1);
	return she_error(vm,
		"not allowed to %s.\n"
		"  This script was not granted %s.\n"
		"  Run it with %s to permit it.",
		what, she_allow_name(need_allow), she_allow_flag(need_allow));
}

bool she_check_allow(struct she_vm *vm, u32 need, const char *what)
{
	if (vm->allow & need)
		return true;
	she_deny(vm, need, what);
	return false;
}

/* ---------------------------------------------------------------- stack */

static void push(struct she_vm *vm, struct she_value v)
{
	if (vm->sp >= vm->stack + SHE_STACK_MAX) {
		she_error(vm, "the value stack overflowed; this usually means runaway recursion");
		return;
	}
	*vm->sp++ = v;
}

static struct she_value pop(struct she_vm *vm)
{
	if (vm->sp <= vm->stack)
		return SHE_NOTHING_V;
	return *--vm->sp;
}

static struct she_value peek(struct she_vm *vm, int back)
{
	if (vm->sp - back - 1 < vm->stack)
		return SHE_NOTHING_V;
	return vm->sp[-1 - back];
}

/* --------------------------------------------------------------- globals */

int she_global_set(struct she_vm *vm, const char *name, struct she_value v)
{
	struct she_obj *key = she_string_new(vm, name, strlen(name));
	if (!key)
		return RK_ENOMEM;
	return she_map_set(vm, vm->globals, key, v);
}

bool she_global_get(struct she_vm *vm, const char *name, struct she_value *out)
{
	struct she_obj *key = she_string_new(vm, name, strlen(name));
	return key && she_map_get(vm->globals, key, out);
}

/* --------------------------------------------------------------- numbers */

/* Arithmetic promotes a whole number to fixed point when mixed, so 3 / 2 in a
 * real context does not silently truncate, but integer arithmetic stays exact
 * when both sides are integers - which is what a script counting things
 * expects. */
static bool arith(struct she_vm *vm, enum she_op op, struct she_value a,
                  struct she_value b, struct she_value *out)
{
	if (a.type == SHE_NUM && b.type == SHE_NUM) {
		s64 x = a.as.n, y = b.as.n, r;
		switch (op) {
		case OP_ADD: r = x + y; break;
		case OP_SUB: r = x - y; break;
		case OP_MUL: r = x * y; break;
		case OP_DIV:
			if (!y) {
				she_error(vm, "cannot divide %lld by zero", (long long)x);
				return false;
			}
			/* An exact division stays a whole number; anything else becomes
			 * a real, so 7/2 is 3.5 rather than 3. */
			if (x % y == 0) {
				r = x / y;
			} else {
				out->type = SHE_REAL;
				out->as.r = kq_div(kq_from_int(x), kq_from_int(y));
				return true;
			}
			break;
		case OP_MOD:
			if (!y) {
				she_error(vm, "cannot take the remainder of %lld divided by zero",
				          (long long)x);
				return false;
			}
			r = x % y;
			break;
		case OP_POW: {
			r = 1;
			for (s64 i = 0; i < y && i < 63; i++)
				r *= x;
			break;
		}
		default: return false;
		}
		*out = SHE_NUM_V(r);
		return true;
	}

	/* Mixed or real: work in Q32.32. */
	kq_t x = a.type == SHE_REAL ? a.as.r : kq_from_int(a.as.n);
	kq_t y = b.type == SHE_REAL ? b.as.r : kq_from_int(b.as.n);
	kq_t r;

	if (a.type != SHE_NUM && a.type != SHE_REAL) {
		she_error(vm, "cannot do arithmetic on %s", she_typename(a.type));
		return false;
	}
	if (b.type != SHE_NUM && b.type != SHE_REAL) {
		she_error(vm, "cannot do arithmetic on %s", she_typename(b.type));
		return false;
	}

	switch (op) {
	case OP_ADD: r = x + y; break;
	case OP_SUB: r = x - y; break;
	case OP_MUL: r = kq_mul(x, y); break;
	case OP_DIV:
		if (!y) {
			she_error(vm, "cannot divide by zero");
			return false;
		}
		r = kq_div(x, y);
		break;
	case OP_MOD: r = y ? x % y : 0; break;
	default:
		she_error(vm, "that operation does not work on real numbers");
		return false;
	}
	out->type = SHE_REAL;
	out->as.r = r;
	return true;
}

static bool compare(struct she_vm *vm, struct she_value a, struct she_value b, int *cmp)
{
	if ((a.type == SHE_NUM || a.type == SHE_REAL) &&
	    (b.type == SHE_NUM || b.type == SHE_REAL)) {
		kq_t x = a.type == SHE_REAL ? a.as.r : kq_from_int(a.as.n);
		kq_t y = b.type == SHE_REAL ? b.as.r : kq_from_int(b.as.n);
		*cmp = x < y ? -1 : (x > y ? 1 : 0);
		return true;
	}
	if (a.type == SHE_TEXT && b.type == SHE_TEXT) {
		*cmp = strcmp(she_string_chars(a.as.o), she_string_chars(b.as.o));
		return true;
	}
	she_error(vm, "cannot compare %s with %s",
	          she_typename(a.type), she_typename(b.type));
	return false;
}

/* ------------------------------------------------------------------ calls */

static bool call_value(struct she_vm *vm, struct she_value callee, u8 argc);

static bool call_function(struct she_vm *vm, struct she_obj *fn, u8 argc)
{
	u8 arity = she_function_arity(fn);
	if (argc != arity) {
		she_error(vm, "%s takes %u argument%s but was given %u",
		          she_function_name(fn), arity, arity == 1 ? "" : "s", argc);
		return false;
	}
	if (vm->nframes >= SHE_FRAMES_MAX) {
		she_error(vm, "too many nested calls (the limit is %u); is this recursion "
		              "missing its base case?", SHE_FRAMES_MAX);
		return false;
	}

	struct she_frame *f = &vm->frames[vm->nframes++];
	f->closure = fn;
	f->ip = she_function_chunk(fn)->code;
	f->slots = vm->sp - argc - 1;
	return true;
}

static bool call_native(struct she_vm *vm, struct she_obj *nat, u8 argc)
{
	const struct she_native *d = she_native_def(nat);
	if (!d)
		return false;

	if (argc < d->min_args || argc > d->max_args) {
		she_error(vm, "%s takes between %u and %u arguments but was given %u",
		          d->name, d->min_args, d->max_args, argc);
		return false;
	}
	if (d->needs_allow && !she_check_allow(vm, d->needs_allow, d->name))
		return false;

	struct she_value out = SHE_NOTHING_V;
	int rc = d->fn(vm, argc, vm->sp - argc, &out);
	vm->sp -= argc + 1;
	push(vm, out);
	return rc == RK_OK && !vm->failed;
}

static bool call_value(struct she_vm *vm, struct she_value callee, u8 argc)
{
	if (callee.type == SHE_FUNCTION)
		return call_function(vm, callee.as.o, argc);
	if (callee.type == SHE_NATIVE)
		return call_native(vm, callee.as.o, argc);

	she_error(vm, "%s is not something that can be called",
	          she_typename(callee.type));
	return false;
}

/* --------------------------------------------------------------- the loop */

#define READ_BYTE()  (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (u16)(frame->ip[-2] | (frame->ip[-1] << 8)))
#define READ_CONST() (chunk->consts[READ_BYTE()])

/* stop_at is the frame count at which the interpreter hands control back. It
 * is 0 for a top-level run and the caller's depth when a native re-enters the
 * VM to invoke a callback, which is how map and filter work without needing a
 * second interpreter or a copy of the stack. */
static int run(struct she_vm *vm, struct she_value *result, u32 stop_at)
{
	struct she_frame *frame = &vm->frames[vm->nframes - 1];
	struct she_chunk *chunk = she_function_chunk(frame->closure);

	for (;;) {
		if (vm->gas == 0) {
			/* Out of budget, but not broken. The caller may top it up and
			 * call she_resume, which is how a long-running script stays
			 * preemptible without threads. */
			return RK_EAGAIN;
		}
		vm->gas--;
		sstats.instructions++;

		u8 op = READ_BYTE();
		switch (op) {
		case OP_NOP: break;
		case OP_CONST:   push(vm, READ_CONST()); break;
		case OP_NOTHING: push(vm, SHE_NOTHING_V); break;
		case OP_TRUE:    push(vm, SHE_BOOL_V(true)); break;
		case OP_FALSE:   push(vm, SHE_BOOL_V(false)); break;
		case OP_POP:     pop(vm); break;
		case OP_DUP:     push(vm, peek(vm, 0)); break;

		case OP_GET_LOCAL: push(vm, frame->slots[READ_BYTE()]); break;
		case OP_SET_LOCAL: frame->slots[READ_BYTE()] = peek(vm, 0); break;

		case OP_GET_GLOBAL: {
			struct she_value key = pop(vm);
			struct she_value v;
			if (!she_map_get(vm->globals, key.as.o, &v)) {
				/* Name errors are where a beginner spends the most time, so
				 * the message says what to do about it. */
				return she_error(vm, "there is no name \"%s\" here.\n"
				                     "  Did you mean to write: let %s = ...",
				                 she_string_chars(key.as.o),
				                 she_string_chars(key.as.o));
			}
			push(vm, v);
			break;
		}
		case OP_DEF_GLOBAL: {
			struct she_value key = pop(vm);
			struct she_value val = pop(vm);
			she_map_set(vm, vm->globals, key.as.o, val);
			break;
		}
		case OP_SET_GLOBAL: {
			struct she_value key = pop(vm);
			struct she_value val = peek(vm, 0);
			struct she_value old;
			if (!she_map_get(vm->globals, key.as.o, &old))
				return she_error(vm, "\"%s\" has not been created yet; "
				                     "write let %s = ... first",
				                 she_string_chars(key.as.o),
				                 she_string_chars(key.as.o));
			she_map_set(vm, vm->globals, key.as.o, val);
			break;
		}

		case OP_GET_INDEX: {
			struct she_value idx = pop(vm);
			struct she_value obj = pop(vm);
			if (obj.type == SHE_LIST) {
				if (idx.type != SHE_NUM)
					return she_error(vm, "a list index must be a whole number, not %s",
					                 she_typename(idx.type));
				s64 i = idx.as.n;
				if (i < 0)
					i += obj.as.o->len;      /* negative indexes count back */
				if (i < 0 || (u32)i >= obj.as.o->len)
					return she_error(vm, "index %lld is outside the list, "
					                     "which has %u items",
					                 (long long)idx.as.n, obj.as.o->len);
				push(vm, she_list_get(obj.as.o, (u32)i));
			} else if (obj.type == SHE_MAP) {
				struct she_value v = SHE_NOTHING_V;
				she_map_get(obj.as.o, idx.as.o, &v);
				push(vm, v);
			} else if (obj.type == SHE_TEXT) {
				s64 i = idx.as.n;
				const char *s = she_string_chars(obj.as.o);
				if (i < 0 || (u32)i >= obj.as.o->len)
					push(vm, SHE_NOTHING_V);
				else
					push(vm, she_obj_value(SHE_TEXT,
					     she_string_new(vm, s + i, 1)));
			} else {
				return she_error(vm, "%s cannot be indexed",
				                 she_typename(obj.type));
			}
			break;
		}
		case OP_GET_FIELD: {
			struct she_value key = pop(vm);
			struct she_value obj = pop(vm);
			struct she_value v = SHE_NOTHING_V;
			if (obj.type == SHE_MAP)
				she_map_get(obj.as.o, key.as.o, &v);
			else if (obj.type == SHE_LIST || obj.type == SHE_TEXT) {
				if (strcmp(she_string_chars(key.as.o), "count") == 0)
					v = SHE_NUM_V(obj.as.o->len);
			}
			push(vm, v);
			break;
		}
		case OP_SET_INDEX: {
			struct she_value val = pop(vm);
			struct she_value idx = pop(vm);
			struct she_value obj = pop(vm);
			if (obj.type == SHE_LIST && idx.type == SHE_NUM)
				she_list_set(obj.as.o, (u32)idx.as.n, val);
			else if (obj.type == SHE_MAP)
				she_map_set(vm, obj.as.o, idx.as.o, val);
			else
				return she_error(vm, "cannot store into %s", she_typename(obj.type));
			push(vm, val);
			break;
		}

		case OP_ADD: case OP_SUB: case OP_MUL:
		case OP_DIV: case OP_MOD: case OP_POW: {
			struct she_value b = pop(vm), a = pop(vm), r;
			/* Adding text concatenates, which is what people write first. */
			if (op == OP_ADD && (a.type == SHE_TEXT || b.type == SHE_TEXT)) {
				char buf[512];
				size_t n = she_value_repr(&a, buf, sizeof(buf));
				n += she_value_repr(&b, buf + n, sizeof(buf) > n ? sizeof(buf) - n : 0);
				push(vm, she_obj_value(SHE_TEXT, she_string_new(vm, buf, n)));
				break;
			}
			if (!arith(vm, (enum she_op)op, a, b, &r))
				return RK_EINVAL;
			push(vm, r);
			break;
		}
		case OP_NEG: {
			struct she_value a = pop(vm);
			if (a.type == SHE_NUM)       push(vm, SHE_NUM_V(-a.as.n));
			else if (a.type == SHE_REAL) { a.as.r = -a.as.r; push(vm, a); }
			else return she_error(vm, "cannot negate %s", she_typename(a.type));
			break;
		}
		case OP_NOT: push(vm, SHE_BOOL_V(!she_truthy(pop(vm)))); break;

		case OP_EQ:  { struct she_value b = pop(vm), a = pop(vm);
		               push(vm, SHE_BOOL_V(she_equal(a, b))); break; }
		case OP_NEQ: { struct she_value b = pop(vm), a = pop(vm);
		               push(vm, SHE_BOOL_V(!she_equal(a, b))); break; }
		case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
			struct she_value b = pop(vm), a = pop(vm);
			int cmp;
			if (!compare(vm, a, b, &cmp))
				return RK_EINVAL;
			bool r = op == OP_LT ? cmp < 0 : op == OP_LE ? cmp <= 0
			       : op == OP_GT ? cmp > 0 : cmp >= 0;
			push(vm, SHE_BOOL_V(r));
			break;
		}
		case OP_CONCAT: {
			struct she_value b = pop(vm), a = pop(vm);
			char buf[512];
			size_t n = she_value_repr(&a, buf, sizeof(buf));
			n += she_value_repr(&b, buf + n, sizeof(buf) > n ? sizeof(buf) - n : 0);
			push(vm, she_obj_value(SHE_TEXT, she_string_new(vm, buf, n)));
			break;
		}

		case OP_JUMP:          { u16 d = READ_SHORT(); frame->ip += d; break; }
		case OP_JUMP_IF_FALSE: { u16 d = READ_SHORT();
		                         if (!she_truthy(peek(vm, 0))) frame->ip += d;
		                         break; }
		case OP_JUMP_IF_TRUE:  { u16 d = READ_SHORT();
		                         if (she_truthy(peek(vm, 0))) frame->ip += d;
		                         break; }
		case OP_LOOP:          { u16 d = READ_SHORT(); frame->ip -= d; break; }

		case OP_CALL: {
			u8 argc = READ_BYTE();
			if (!call_value(vm, peek(vm, argc), argc))
				return RK_EINVAL;
			frame = &vm->frames[vm->nframes - 1];
			chunk = she_function_chunk(frame->closure);
			break;
		}
		case OP_PIPE: {
			/* The stack holds value, callee, then any extra arguments. The
			 * call wants callee, value, extras - so this is a swap of the
			 * bottom two, and nothing more. */
			u8 extra = READ_BYTE();
			u8 argc = (u8)(extra + 1);
			struct she_value *base = vm->sp - extra - 2;
			struct she_value tmp = base[0];
			base[0] = base[1];
			base[1] = tmp;

			if (!call_value(vm, base[0], argc))
				return RK_EINVAL;
			frame = &vm->frames[vm->nframes - 1];
			chunk = she_function_chunk(frame->closure);
			break;
		}
		case OP_RETURN: {
			struct she_value r = pop(vm);
			vm->nframes--;
			if (vm->nframes == stop_at) {
				vm->sp = frame->slots;
				if (result)
					*result = r;
				return RK_OK;
			}
			vm->sp = frame->slots;
			push(vm, r);
			frame = &vm->frames[vm->nframes - 1];
			chunk = she_function_chunk(frame->closure);
			break;
		}

		case OP_MAKE_LIST: {
			u8 n = READ_BYTE();
			struct she_obj *l = she_list_new(vm, n);
			if (!l)
				return RK_ENOMEM;
			for (u8 i = 0; i < n; i++)
				she_list_push(vm, l, vm->sp[-(int)n + i]);
			vm->sp -= n;
			push(vm, she_obj_value(SHE_LIST, l));
			break;
		}
		case OP_MAKE_MAP: {
			u8 n = READ_BYTE();
			struct she_obj *m = she_map_new(vm);
			if (!m)
				return RK_ENOMEM;
			for (u8 i = 0; i < n; i++) {
				struct she_value val = vm->sp[-2 * (int)n + 2 * i + 1];
				struct she_value key = vm->sp[-2 * (int)n + 2 * i];
				she_map_set(vm, m, key.as.o, val);
			}
			vm->sp -= 2 * n;
			push(vm, she_obj_value(SHE_MAP, m));
			break;
		}
		case OP_MAKE_RANGE: {
			struct she_value stop = pop(vm), start = pop(vm);
			if (start.type != SHE_NUM || stop.type != SHE_NUM)
				return she_error(vm, "a range needs two whole numbers");
			struct she_obj *r = she_range_new(vm, start.as.n, stop.as.n + 1, 1);
			if (!r)
				return RK_ENOMEM;
			push(vm, she_obj_value(SHE_RANGE, r));
			break;
		}
		case OP_ITER_NEXT: {
			u8 slot = READ_BYTE();
			struct she_value it = frame->slots[slot];
			struct she_value ix = frame->slots[slot + 1];
			u32 i = (u32)ix.as.n;

			u32 len = 0;
			if (it.type == SHE_LIST || it.type == SHE_RANGE || it.type == SHE_TEXT ||
			    it.type == SHE_MAP)
				len = it.as.o->len;
			else
				return she_error(vm, "cannot loop over %s", she_typename(it.type));

			if (i >= len) {
				push(vm, SHE_BOOL_V(false));
				break;
			}
			frame->slots[slot + 1] = SHE_NUM_V((s64)i + 1);

			struct she_value item = SHE_NOTHING_V;
			if (it.type == SHE_LIST) {
				item = she_list_get(it.as.o, i);
			} else if (it.type == SHE_RANGE) {
				item = SHE_NUM_V(she_range_at(it.as.o, i));
			} else if (it.type == SHE_TEXT) {
				item = she_obj_value(SHE_TEXT,
				       she_string_new(vm, she_string_chars(it.as.o) + i, 1));
			} else {
				/* Maps iterate over their keys. */
				u32 cursor = 0, seen = 0;
				struct she_obj *k;
				struct she_value v;
				while (she_map_next(it.as.o, &cursor, &k, &v)) {
					if (seen++ == i) {
						item = she_obj_value(SHE_TEXT, k);
						break;
					}
				}
			}
			push(vm, item);
			push(vm, SHE_BOOL_V(true));
			break;
		}

		case OP_SAY: {
			struct she_value v = pop(vm);
			char buf[1024];
			size_t n = she_value_repr(&v, buf, sizeof(buf));
			if (n >= sizeof(buf))
				n = sizeof(buf) - 1;
			if (vm->write_out) {
				vm->write_out(vm->io_ctx, buf, n);
				vm->write_out(vm->io_ctx, "\n", 1);
			} else {
				rk_console_write(buf, n);
				rk_console_putc('\n');
			}
			break;
		}
		case OP_ASK: {
			struct she_value prompt = pop(vm);
			if (prompt.type != SHE_NOTHING) {
				char buf[256];
				size_t n = she_value_repr(&prompt, buf, sizeof(buf));
				if (vm->write_out)
					vm->write_out(vm->io_ctx, buf, n);
				else
					rk_console_write(buf, n);
				rk_console_putc(' ');
			}
			char line[256];
			int n = vm->read_line ? vm->read_line(vm->io_ctx, line, sizeof(line))
			                      : rk_console_readline(line, sizeof(line));
			if (n < 0)
				n = 0;
			push(vm, she_obj_value(SHE_TEXT, she_string_new(vm, line, (size_t)n)));
			break;
		}

		case OP_REQUIRE_CAP: {
			u8 idx = READ_BYTE();
			if (!she_check_allow(vm, (u32)1 << idx, "use a protected operation"))
				return RK_EACCES;
			break;
		}
		case OP_HALT:
			return RK_OK;

		default:
			return she_error(vm, "internal error: unknown instruction %u", op);
		}

		if (vm->failed)
			return RK_EINVAL;
	}
}

/* ------------------------------------------------------------------- API */

void she_vm_init(struct she_vm *vm, struct capspace *caps, u32 allow)
{
	memset(vm, 0, sizeof(*vm));
	vm->sp = vm->stack;
	list_init(&vm->objects);
	vm->caps  = caps;
	vm->allow = allow;
	vm->gas   = vm->gas_limit = 50000000;   /* generous but finite */
	vm->mem_limit = 8u << 20;
	vm->id = __atomic_add_fetch(&next_vm_id, 1, __ATOMIC_SEQ_CST) - 1;

	vm->globals = she_map_new(vm);

	struct graph_node *n = rk_graph_node_create(GNODE_SCRIPT, "she-vm", NULL, vm);
	if (n) {
		rk_graph_set_u64(n, "vm", vm->id);
		rk_graph_set_u64(n, "allow", allow);
		vm->graph_node = n->id;
	}
	sstats.vms_created++;
}

void she_vm_set_limits(struct she_vm *vm, u64 gas, u64 mem_bytes)
{
	if (gas)
		vm->gas = vm->gas_limit = gas;
	if (mem_bytes)
		vm->mem_limit = mem_bytes;
}

void she_vm_set_io(struct she_vm *vm, void *ctx,
                   void (*out)(void *, const char *, size_t),
                   int (*in)(void *, char *, size_t))
{
	vm->io_ctx = ctx;
	vm->write_out = out;
	vm->read_line = in;
}

void she_vm_free(struct she_vm *vm)
{
	she_object_free_all(vm);
	vm->globals = NULL;
	vm->sp = vm->stack;
	vm->nframes = 0;
}

int she_run(struct she_vm *vm, struct she_obj *fn, struct she_value *result)
{
	if (!fn)
		return RK_EINVAL;

	vm->sp = vm->stack;
	vm->nframes = 0;
	vm->failed = false;

	push(vm, she_obj_value(SHE_FUNCTION, fn));
	if (!call_function(vm, fn, 0))
		return RK_EINVAL;

	int rc = run(vm, result, 0);
	if (rc == RK_EAGAIN)
		sstats.gas_exhausted++;
	return rc;
}

int she_resume(struct she_vm *vm, struct she_value *result)
{
	if (!vm->nframes)
		return RK_ENOENT;
	return run(vm, result, 0);
}

/* Call a SHE value from inside a native, on the current stack. Used by map,
 * filter and anything else that takes a function argument. */
int she_call_now(struct she_vm *vm, struct she_value fn, struct she_value *args,
                 u8 argc, struct she_value *out)
{
	u32 base = vm->nframes;

	push(vm, fn);
	for (u8 i = 0; i < argc; i++)
		push(vm, args[i]);

	if (!call_value(vm, fn, argc))
		return RK_EINVAL;

	/* A native callee has already run to completion and left its result. */
	if (fn.type == SHE_NATIVE) {
		if (out)
			*out = pop(vm);
		return vm->failed ? RK_EINVAL : RK_OK;
	}
	return run(vm, out, base);
}

int she_eval(struct she_vm *vm, const char *source, const char *origin,
             struct she_value *result)
{
	struct she_obj *fn = NULL;
	int rc = she_compile(vm, source, origin, &fn);
	sstats.scripts_compiled++;
	if (rc != RK_OK) {
		sstats.compile_errors++;
		return rc;
	}
	return she_run(vm, fn, result);
}

void she_stats(struct she_stats *out)
{
	*out = sstats;
}
