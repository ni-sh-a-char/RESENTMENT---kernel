/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - SHE values and heap objects.
 *
 * Every object belongs to exactly one VM and is charged against that VM's
 * memory budget. That is what makes it safe to run an untrusted script inside
 * the kernel: it cannot allocate past its limit, and when the VM is torn down
 * every object it ever made goes with it, whether or not the script was
 * well-behaved about references.
 *
 * Numbers are 64-bit integers and Q32.32 fixed point rather than doubles. The
 * kernel avoids the FPU outside the AI subsystem, and the runtime graph
 * promises bit-identical replay across architectures - a promise IEEE754
 * cannot keep once libm is involved.
 */
#include <rk/she.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/crypto.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "she"

/* Object layouts. The header is first in each so a struct she_obj pointer can
 * be cast up once the type tag has been checked. */

struct she_string {
	struct she_obj o;
	u64  hash;
	char data[];
};

struct she_list {
	struct she_obj    o;
	struct she_value *items;
	u32               cap;
};

struct she_mapent {
	struct she_string *key;
	struct she_value   val;
	bool               used, tomb;
};

struct she_map {
	struct she_obj     o;
	struct she_mapent *entries;
	u32                cap;
};

struct she_function {
	struct she_obj    o;
	struct she_chunk  chunk;
	u8                arity;
	u8                ncaptures;
	char              name[32];
	struct she_value  captures[8];
};

struct she_native_obj {
	struct she_obj      o;
	const struct she_native *def;
};

struct she_range_obj {
	struct she_obj o;
	s64 start, stop, step;
};

static const char *const typenames[SHE_VALTYPE_COUNT] = {
	"nothing", "yes/no", "number", "real", "text", "list", "map",
	"range", "function", "builtin", "capability", "tensor"
};

const char *she_typename(enum she_valtype t)
{
	return (unsigned)t < SHE_VALTYPE_COUNT ? typenames[t] : "?";
}

/* --------------------------------------------------------------- budget */

static void *vm_alloc(struct she_vm *vm, size_t n)
{
	if (vm->mem_used + n > vm->mem_limit) {
		she_error(vm, "out of memory: this script may use %llu bytes and has used %llu",
		          (unsigned long long)vm->mem_limit, (unsigned long long)vm->mem_used);
		return NULL;
	}
	void *p = kzalloc(n);
	if (p)
		vm->mem_used += n;
	return p;
}

static void vm_free(struct she_vm *vm, void *p, size_t n)
{
	if (!p)
		return;
	vm->mem_used = vm->mem_used > n ? vm->mem_used - n : 0;
	kfree(p);
}

static struct she_obj *obj_new(struct she_vm *vm, enum she_valtype type, size_t bytes)
{
	struct she_obj *o = vm_alloc(vm, bytes);
	if (!o)
		return NULL;
	o->type = (u8)type;
	o->refcount = 1;
	o->vm = vm;
	list_add(&o->link, &vm->objects);
	return o;
}

/* ---------------------------------------------------------------- text */

/* Strings are interned: equality becomes a pointer comparison and a map key
 * lookup does not have to compare bytes on the common path. */
struct she_obj *she_string_new(struct she_vm *vm, const char *s, size_t len)
{
	u64 h = rk_fnv1a(s, len);

	struct she_obj *o;
	list_for_each_entry(o, &vm->objects, link) {
		if (o->type != SHE_TEXT)
			continue;
		struct she_string *str = (struct she_string *)o;
		if (str->hash == h && o->len == len && memcmp(str->data, s, len) == 0) {
			o->refcount++;
			return o;
		}
	}

	struct she_string *str = (struct she_string *)obj_new(vm, SHE_TEXT,
	                                                      sizeof(*str) + len + 1);
	if (!str)
		return NULL;
	memcpy(str->data, s, len);
	str->data[len] = '\0';
	str->hash = h;
	str->o.len = (u32)len;
	return &str->o;
}

const char *she_string_chars(const struct she_obj *o)
{
	return o && o->type == SHE_TEXT ? ((const struct she_string *)o)->data : "";
}

/* ---------------------------------------------------------------- list */

struct she_obj *she_list_new(struct she_vm *vm, u32 capacity)
{
	struct she_list *l = (struct she_list *)obj_new(vm, SHE_LIST, sizeof(*l));
	if (!l)
		return NULL;
	if (capacity) {
		l->items = vm_alloc(vm, capacity * sizeof(struct she_value));
		if (!l->items) {
			return &l->o;   /* empty but valid; the error is already set */
		}
		l->cap = capacity;
	}
	return &l->o;
}

int she_list_push(struct she_vm *vm, struct she_obj *o, struct she_value v)
{
	if (!o || o->type != SHE_LIST)
		return RK_EINVAL;
	struct she_list *l = (struct she_list *)o;

	if (o->len >= l->cap) {
		u32 cap = l->cap ? l->cap * 2 : 8;
		struct she_value *fresh = vm_alloc(vm, cap * sizeof(struct she_value));
		if (!fresh)
			return RK_ENOMEM;
		if (l->items) {
			memcpy(fresh, l->items, o->len * sizeof(struct she_value));
			vm_free(vm, l->items, l->cap * sizeof(struct she_value));
		}
		l->items = fresh;
		l->cap = cap;
	}
	l->items[o->len++] = v;
	return RK_OK;
}

struct she_value she_list_get(struct she_obj *o, u32 i)
{
	struct she_list *l = (struct she_list *)o;
	if (!o || o->type != SHE_LIST || i >= o->len)
		return SHE_NOTHING_V;
	return l->items[i];
}

int she_list_set(struct she_obj *o, u32 i, struct she_value v)
{
	struct she_list *l = (struct she_list *)o;
	if (!o || o->type != SHE_LIST || i >= o->len)
		return RK_ERANGE;
	l->items[i] = v;
	return RK_OK;
}

/* ----------------------------------------------------------------- map */

/* Open addressing with linear probing, resized at 75% load. Tombstones rather
 * than backward shifting, because a script that deletes in a loop should not
 * pay O(n) per delete. */
static struct she_mapent *map_find(struct she_mapent *entries, u32 cap,
                                   struct she_string *key)
{
	if (!cap)
		return NULL;
	u32 idx = (u32)(key->hash % cap);
	struct she_mapent *tomb = NULL;

	for (;;) {
		struct she_mapent *e = &entries[idx];
		if (!e->used) {
			if (!e->tomb)
				return tomb ? tomb : e;
			if (!tomb)
				tomb = e;
		} else if (e->key == key) {
			return e;
		}
		idx = (idx + 1) % cap;
	}
}

struct she_obj *she_map_new(struct she_vm *vm)
{
	struct she_map *m = (struct she_map *)obj_new(vm, SHE_MAP, sizeof(*m));
	return m ? &m->o : NULL;
}

static int map_grow(struct she_vm *vm, struct she_map *m)
{
	u32 cap = m->cap ? m->cap * 2 : 16;
	struct she_mapent *fresh = vm_alloc(vm, cap * sizeof(struct she_mapent));
	if (!fresh)
		return RK_ENOMEM;

	u32 live = 0;
	for (u32 i = 0; i < m->cap; i++) {
		if (!m->entries[i].used)
			continue;
		struct she_mapent *dst = map_find(fresh, cap, m->entries[i].key);
		*dst = m->entries[i];
		dst->tomb = false;
		live++;
	}
	if (m->entries)
		vm_free(vm, m->entries, m->cap * sizeof(struct she_mapent));
	m->entries = fresh;
	m->cap = cap;
	m->o.len = live;
	return RK_OK;
}

int she_map_set(struct she_vm *vm, struct she_obj *o, struct she_obj *key,
                struct she_value val)
{
	if (!o || o->type != SHE_MAP || !key || key->type != SHE_TEXT)
		return RK_EINVAL;
	struct she_map *m = (struct she_map *)o;

	if (m->o.len + 1 > m->cap * 3 / 4) {
		int r = map_grow(vm, m);
		if (r != RK_OK)
			return r;
	}
	struct she_mapent *e = map_find(m->entries, m->cap, (struct she_string *)key);
	if (!e->used)
		m->o.len++;
	e->key  = (struct she_string *)key;
	e->val  = val;
	e->used = true;
	e->tomb = false;
	return RK_OK;
}

bool she_map_get(struct she_obj *o, struct she_obj *key, struct she_value *out)
{
	if (!o || o->type != SHE_MAP || !key || key->type != SHE_TEXT)
		return false;
	struct she_map *m = (struct she_map *)o;
	if (!m->cap)
		return false;

	struct she_mapent *e = map_find(m->entries, m->cap, (struct she_string *)key);
	if (!e->used)
		return false;
	*out = e->val;
	return true;
}

bool she_map_del(struct she_obj *o, struct she_obj *key)
{
	if (!o || o->type != SHE_MAP)
		return false;
	struct she_map *m = (struct she_map *)o;
	if (!m->cap)
		return false;

	struct she_mapent *e = map_find(m->entries, m->cap, (struct she_string *)key);
	if (!e->used)
		return false;
	e->used = false;
	e->tomb = true;
	if (m->o.len)
		m->o.len--;
	return true;
}

/* Iterate for the interpreter and for `for each`. Returns false when done. */
bool she_map_next(struct she_obj *o, u32 *cursor, struct she_obj **key,
                  struct she_value *val)
{
	if (!o || o->type != SHE_MAP)
		return false;
	struct she_map *m = (struct she_map *)o;
	while (*cursor < m->cap) {
		struct she_mapent *e = &m->entries[(*cursor)++];
		if (e->used) {
			*key = &e->key->o;
			*val = e->val;
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------- functions */

struct she_obj *she_function_new(struct she_vm *vm, const char *name, u8 arity)
{
	struct she_function *f = (struct she_function *)obj_new(vm, SHE_FUNCTION, sizeof(*f));
	if (!f)
		return NULL;
	f->arity = arity;
	strlcpy(f->name, name ? name : "anonymous", sizeof(f->name));
	return &f->o;
}

struct she_chunk *she_function_chunk(struct she_obj *o)
{
	return o && o->type == SHE_FUNCTION ? &((struct she_function *)o)->chunk : NULL;
}

u8 she_function_arity(struct she_obj *o)
{
	return o && o->type == SHE_FUNCTION ? ((struct she_function *)o)->arity : 0;
}

void she_function_set_arity(struct she_obj *o, u8 arity)
{
	if (o && o->type == SHE_FUNCTION)
		((struct she_function *)o)->arity = arity;
}

const char *she_function_name(struct she_obj *o)
{
	return o && o->type == SHE_FUNCTION ? ((struct she_function *)o)->name : "?";
}

/* Value capture, not reference capture. A lambda takes a copy of the enclosing
 * locals it mentions at the moment it is created. That is the semantics most
 * scripts expect, and it removes an entire class of lifetime bug from a
 * language running in ring 0. */
int she_function_capture(struct she_obj *o, u8 slot, struct she_value v)
{
	if (!o || o->type != SHE_FUNCTION || slot >= 8)
		return RK_ERANGE;
	struct she_function *f = (struct she_function *)o;
	f->captures[slot] = v;
	if (slot + 1 > f->ncaptures)
		f->ncaptures = slot + 1;
	return RK_OK;
}

struct she_value she_function_captured(struct she_obj *o, u8 slot)
{
	if (!o || o->type != SHE_FUNCTION || slot >= 8)
		return SHE_NOTHING_V;
	return ((struct she_function *)o)->captures[slot];
}

struct she_obj *she_native_new(struct she_vm *vm, const struct she_native *def)
{
	struct she_native_obj *n = (struct she_native_obj *)obj_new(vm, SHE_NATIVE, sizeof(*n));
	if (!n)
		return NULL;
	n->def = def;
	return &n->o;
}

const struct she_native *she_native_def(struct she_obj *o)
{
	return o && o->type == SHE_NATIVE ? ((struct she_native_obj *)o)->def : NULL;
}

struct she_obj *she_range_new(struct she_vm *vm, s64 start, s64 stop, s64 step)
{
	struct she_range_obj *r = (struct she_range_obj *)obj_new(vm, SHE_RANGE, sizeof(*r));
	if (!r)
		return NULL;
	r->start = start;
	r->stop  = stop;
	r->step  = step ? step : 1;
	s64 span = stop - start;
	r->o.len = (u32)(span > 0 ? (span + r->step - 1) / r->step : 0);
	return &r->o;
}

s64 she_range_at(struct she_obj *o, u32 i)
{
	struct she_range_obj *r = (struct she_range_obj *)o;
	return r->start + (s64)i * r->step;
}

/* ------------------------------------------------------------- chunks */

int she_chunk_write(struct she_vm *vm, struct she_chunk *c, u8 byte, u32 line)
{
	if (c->len >= c->cap) {
		u32 cap = c->cap ? c->cap * 2 : 64;
		u8  *code = vm_alloc(vm, cap);
		u32 *lines = vm_alloc(vm, cap * sizeof(u32));
		if (!code || !lines)
			return RK_ENOMEM;
		if (c->code) {
			memcpy(code, c->code, c->len);
			memcpy(lines, c->lines, c->len * sizeof(u32));
			vm_free(vm, c->code, c->cap);
			vm_free(vm, c->lines, c->cap * sizeof(u32));
		}
		c->code = code;
		c->lines = lines;
		c->cap = cap;
	}
	c->code[c->len] = byte;
	c->lines[c->len] = line;
	c->len++;
	return RK_OK;
}

int she_chunk_constant(struct she_vm *vm, struct she_chunk *c, struct she_value v)
{
	/* Deduplicate: a loop body that mentions the same literal repeatedly
	 * should not grow the constant table each time. */
	for (u32 i = 0; i < c->nconsts; i++) {
		if (c->consts[i].type != v.type)
			continue;
		if (v.type == SHE_NUM && c->consts[i].as.n == v.as.n)
			return (int)i;
		if (v.type == SHE_TEXT && c->consts[i].as.o == v.as.o)
			return (int)i;
	}

	if (c->nconsts >= c->consts_cap) {
		u32 cap = c->consts_cap ? c->consts_cap * 2 : 16;
		struct she_value *fresh = vm_alloc(vm, cap * sizeof(struct she_value));
		if (!fresh)
			return -1;
		if (c->consts) {
			memcpy(fresh, c->consts, c->nconsts * sizeof(struct she_value));
			vm_free(vm, c->consts, c->consts_cap * sizeof(struct she_value));
		}
		c->consts = fresh;
		c->consts_cap = cap;
	}
	c->consts[c->nconsts] = v;
	return (int)c->nconsts++;
}

/* ----------------------------------------------------------- formatting */

size_t she_value_repr(const struct she_value *v, char *buf, size_t n)
{
	if (!v)
		return (size_t)snprintf(buf, n, "nothing");

	switch (v->type) {
	case SHE_NOTHING: return (size_t)snprintf(buf, n, "nothing");
	case SHE_BOOL:    return (size_t)snprintf(buf, n, v->as.b ? "yes" : "no");
	case SHE_NUM:     return (size_t)snprintf(buf, n, "%lld", (long long)v->as.n);
	case SHE_REAL: {
		/* Q32.32 printed as an integer part and six decimal places, computed
		 * with integers so this stays FPU-free. */
		s64 whole = v->as.r >> KQ_SHIFT;
		u64 frac = (u64)(v->as.r & (KQ_ONE - 1));
		u64 micros = (frac * 1000000ull) >> KQ_SHIFT;
		return (size_t)snprintf(buf, n, "%lld.%06llu",
		                        (long long)whole, (unsigned long long)micros);
	}
	case SHE_TEXT:    return (size_t)snprintf(buf, n, "%s", she_string_chars(v->as.o));
	case SHE_CAP:     return (size_t)snprintf(buf, n, "<capability %d>", (int)v->as.cap);
	case SHE_FUNCTION:
		return (size_t)snprintf(buf, n, "<function %s>", she_function_name(v->as.o));
	case SHE_NATIVE: {
		const struct she_native *d = she_native_def(v->as.o);
		return (size_t)snprintf(buf, n, "<builtin %s>", d ? d->name : "?");
	}
	case SHE_RANGE: {
		struct she_range_obj *r = (struct she_range_obj *)v->as.o;
		return (size_t)snprintf(buf, n, "%lld to %lld",
		                        (long long)r->start, (long long)r->stop);
	}
	case SHE_LIST: {
		size_t len = (size_t)snprintf(buf, n, "[");
		for (u32 i = 0; i < v->as.o->len; i++) {
			struct she_value item = she_list_get(v->as.o, i);
			if (i)
				len += (size_t)snprintf(buf + len, n > len ? n - len : 0, ", ");
			len += she_value_repr(&item, buf + len, n > len ? n - len : 0);
		}
		return len + (size_t)snprintf(buf + len, n > len ? n - len : 0, "]");
	}
	case SHE_MAP: {
		size_t len = (size_t)snprintf(buf, n, "{");
		u32 cursor = 0;
		struct she_obj *k;
		struct she_value val;
		bool first = true;
		while (she_map_next(v->as.o, &cursor, &k, &val)) {
			len += (size_t)snprintf(buf + len, n > len ? n - len : 0, "%s%s: ",
			                        first ? "" : ", ", she_string_chars(k));
			first = false;
			len += she_value_repr(&val, buf + len, n > len ? n - len : 0);
		}
		return len + (size_t)snprintf(buf + len, n > len ? n - len : 0, "}");
	}
	default: return (size_t)snprintf(buf, n, "<%s>", she_typename(v->type));
	}
}

bool she_truthy(struct she_value v)
{
	switch (v.type) {
	case SHE_NOTHING: return false;
	case SHE_BOOL:    return v.as.b;
	case SHE_NUM:     return v.as.n != 0;
	case SHE_REAL:    return v.as.r != 0;
	case SHE_TEXT:
	case SHE_LIST:
	case SHE_MAP:     return v.as.o && v.as.o->len > 0;
	default:          return true;
	}
}

bool she_equal(struct she_value a, struct she_value b)
{
	if (a.type != b.type) {
		/* A whole number and a real that happen to be equal compare equal;
		 * anything else across types does not. */
		if (a.type == SHE_NUM && b.type == SHE_REAL)
			return kq_from_int(a.as.n) == b.as.r;
		if (a.type == SHE_REAL && b.type == SHE_NUM)
			return a.as.r == kq_from_int(b.as.n);
		return false;
	}
	switch (a.type) {
	case SHE_NOTHING: return true;
	case SHE_BOOL:    return a.as.b == b.as.b;
	case SHE_NUM:     return a.as.n == b.as.n;
	case SHE_REAL:    return a.as.r == b.as.r;
	case SHE_CAP:     return a.as.cap == b.as.cap;
	default:          return a.as.o == b.as.o;   /* strings are interned */
	}
}

void she_object_free_all(struct she_vm *vm)
{
	struct she_obj *o, *tmp;
	list_for_each_entry_safe(o, tmp, &vm->objects, link) {
		list_del(&o->link);
		switch (o->type) {
		case SHE_LIST: {
			struct she_list *l = (struct she_list *)o;
			if (l->items)
				kfree(l->items);
			break;
		}
		case SHE_MAP: {
			struct she_map *m = (struct she_map *)o;
			if (m->entries)
				kfree(m->entries);
			break;
		}
		case SHE_FUNCTION: {
			struct she_function *f = (struct she_function *)o;
			kfree(f->chunk.code);
			kfree(f->chunk.lines);
			kfree(f->chunk.consts);
			break;
		}
		default:
			break;
		}
		kfree(o);
	}
	vm->mem_used = 0;
}
