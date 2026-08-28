/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - SHE internals shared between the value model, the
 * compiler, the VM and the standard library. Not part of the kernel API.
 */
#pragma once

#include <rk/she.h>
#include <rk/string.h>

/* ------------------------------------------------------------- objects */

struct she_obj *she_string_new(struct she_vm *vm, const char *s, size_t len);
const char     *she_string_chars(const struct she_obj *o);

struct she_obj  *she_list_new(struct she_vm *vm, u32 capacity);
int              she_list_push(struct she_vm *vm, struct she_obj *o, struct she_value v);
struct she_value she_list_get(struct she_obj *o, u32 i);
int              she_list_set(struct she_obj *o, u32 i, struct she_value v);

struct she_obj *she_map_new(struct she_vm *vm);
int             she_map_set(struct she_vm *vm, struct she_obj *o, struct she_obj *key,
                            struct she_value val);
bool            she_map_get(struct she_obj *o, struct she_obj *key, struct she_value *out);
bool            she_map_del(struct she_obj *o, struct she_obj *key);
bool            she_map_next(struct she_obj *o, u32 *cursor, struct she_obj **key,
                             struct she_value *val);

struct she_obj   *she_function_new(struct she_vm *vm, const char *name, u8 arity);
struct she_chunk *she_function_chunk(struct she_obj *o);
u8                she_function_arity(struct she_obj *o);
void              she_function_set_arity(struct she_obj *o, u8 arity);
const char       *she_function_name(struct she_obj *o);
int               she_function_capture(struct she_obj *o, u8 slot, struct she_value v);
struct she_value  she_function_captured(struct she_obj *o, u8 slot);

struct she_obj *she_native_new(struct she_vm *vm, const struct she_native *def);
const struct she_native *she_native_def(struct she_obj *o);

struct she_obj *she_range_new(struct she_vm *vm, s64 start, s64 stop, s64 step);
s64             she_range_at(struct she_obj *o, u32 i);

int  she_chunk_write(struct she_vm *vm, struct she_chunk *c, u8 byte, u32 line);
int  she_chunk_constant(struct she_vm *vm, struct she_chunk *c, struct she_value v);

bool she_truthy(struct she_value v);
bool she_equal(struct she_value a, struct she_value b);
void she_object_free_all(struct she_vm *vm);

/* Convenience constructors used by the compiler and the standard library. */
static inline struct she_value she_obj_value(enum she_valtype t, struct she_obj *o)
{
	struct she_value v = { .type = (u8)t };
	v.as.o = o;
	return v;
}

static inline struct she_value she_text(struct she_vm *vm, const char *s)
{
	struct she_obj *o = she_string_new(vm, s, strlen(s));
	return o ? she_obj_value(SHE_TEXT, o) : SHE_NOTHING_V;
}

/* The lookup the VM uses for globals, exposed so the shell can pre-seed a
 * session and the standard library can register itself. */
int  she_global_set(struct she_vm *vm, const char *name, struct she_value v);
bool she_global_get(struct she_vm *vm, const char *name, struct she_value *out);

/* Capability gate. Every native that touches the outside world calls this
 * first, and the diagnostic it produces names the exact flag that grants it. */
bool she_check_allow(struct she_vm *vm, u32 need, const char *what);

/* Invoke a SHE value from inside a native, reusing the current stack. */
int  she_call_now(struct she_vm *vm, struct she_value fn, struct she_value *args,
                  u8 argc, struct she_value *out);

/* Bind the registered builtins into one VM's globals. */
void she_stdlib_bind(struct she_vm *vm);
