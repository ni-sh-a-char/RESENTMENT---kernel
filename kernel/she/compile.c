/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - SHE lexer and compiler.
 *
 * Single pass, source straight to bytecode, no syntax tree. That is not just
 * about speed: it bounds memory, which matters when the thing being compiled
 * arrived from a shell prompt inside the kernel. A tree-building parser can be
 * made to allocate proportional to nesting depth by a hostile input; this
 * cannot, because the only stack it uses is its own recursion, and that is
 * capped explicitly.
 *
 * Error messages are the other reason SHE exists. Every diagnostic names the
 * line, quotes what was found, says what was expected, and where the problem
 * is a missing permission it prints the exact flag that grants it.
 */
#include "she_internal.h"
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "she"

#define MAX_NEST   64     /* recursion cap: refuse deeper rather than overflow */
#define MAX_LOCALS 128

/* ---------------------------------------------------------------- tokens */

enum tok {
	T_EOF = 0, T_ERROR,
	T_NUMBER, T_REAL, T_TEXT, T_IDENT,
	T_LET, T_SAY, T_ASK, T_IF, T_THEN, T_ELSE, T_END,
	T_WHILE, T_REPEAT, T_UNTIL, T_FOR, T_EACH, T_IN,
	T_FUN, T_RETURN, T_BREAK, T_SKIP, T_MATCH, T_WHEN,
	T_AND, T_OR, T_NOT, T_IS, T_TRUE, T_FALSE, T_NOTHING, T_TO,
	T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET,
	T_EQ, T_NEQ, T_LT, T_LE, T_GT, T_GE, T_ASSIGN,
	T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
	T_COMMA, T_COLON, T_DOT, T_ARROW, T_PIPE,
	T_NEWLINE
};

struct token {
	enum tok    kind;
	const char *start;
	u32         len;
	u32         line;
	s64         number;
	kq_t        real;
};

struct lexer {
	const char *src;
	const char *p;
	u32         line;
};

static const struct { const char *word; enum tok kind; } keywords[] = {
	{ "let", T_LET }, { "say", T_SAY }, { "ask", T_ASK },
	{ "if", T_IF }, { "then", T_THEN }, { "else", T_ELSE }, { "end", T_END },
	{ "while", T_WHILE }, { "repeat", T_REPEAT }, { "until", T_UNTIL },
	{ "for", T_FOR }, { "each", T_EACH }, { "in", T_IN },
	{ "fun", T_FUN }, { "return", T_RETURN }, { "break", T_BREAK },
	{ "skip", T_SKIP }, { "match", T_MATCH }, { "when", T_WHEN },
	{ "and", T_AND }, { "or", T_OR }, { "not", T_NOT }, { "is", T_IS },
	{ "true", T_TRUE }, { "yes", T_TRUE },
	{ "false", T_FALSE }, { "no", T_FALSE },
	{ "nothing", T_NOTHING }, { "to", T_TO },
};

static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static void skip_space(struct lexer *l)
{
	for (;;) {
		char c = *l->p;
		if (c == ' ' || c == '\t' || c == '\r') {
			l->p++;
		} else if (c == '#') {
			while (*l->p && *l->p != '\n')
				l->p++;
		} else {
			return;
		}
	}
}

static struct token lex_next(struct lexer *l)
{
	skip_space(l);

	struct token t = { T_EOF, l->p, 0, l->line, 0, 0 };
	char c = *l->p;
	if (!c)
		return t;

	t.start = l->p;

	if (c == '\n') {
		l->p++;
		l->line++;
		t.kind = T_NEWLINE;
		t.len = 1;
		return t;
	}

	if (is_digit(c)) {
		s64 whole = 0;
		while (is_digit(*l->p))
			whole = whole * 10 + (*l->p++ - '0');

		if (*l->p == '.' && is_digit(l->p[1])) {
			l->p++;
			/* Fixed point, not floating: accumulate the fraction as a
			 * numerator over a power of ten and convert once. */
			u64 num = 0, den = 1;
			while (is_digit(*l->p) && den < 1000000000ull) {
				num = num * 10 + (u64)(*l->p++ - '0');
				den *= 10;
			}
			t.kind = T_REAL;
			t.real = kq_from_int(whole) + (kq_t)((num << KQ_SHIFT) / den);
		} else {
			t.kind = T_NUMBER;
			t.number = whole;
		}
		t.len = (u32)(l->p - t.start);
		return t;
	}

	if (is_alpha(c)) {
		while (is_alpha(*l->p) || is_digit(*l->p))
			l->p++;
		t.len = (u32)(l->p - t.start);
		t.kind = T_IDENT;
		for (size_t i = 0; i < ARRAY_SIZE(keywords); i++) {
			if (strlen(keywords[i].word) == t.len &&
			    strncmp(t.start, keywords[i].word, t.len) == 0) {
				t.kind = keywords[i].kind;
				break;
			}
		}
		return t;
	}

	if (c == '"') {
		l->p++;
		t.start = l->p;
		while (*l->p && *l->p != '"') {
			if (*l->p == '\\' && l->p[1])
				l->p++;
			if (*l->p == '\n')
				l->line++;
			l->p++;
		}
		t.len = (u32)(l->p - t.start);
		if (*l->p == '"')
			l->p++;
		t.kind = T_TEXT;
		return t;
	}

	l->p++;
	t.len = 1;
	switch (c) {
	case '+': t.kind = T_PLUS; break;
	case '-':
		if (*l->p == '>') { l->p++; t.kind = T_ARROW; t.len = 2; }
		else t.kind = T_MINUS;
		break;
	case '*': t.kind = T_STAR; break;
	case '/': t.kind = T_SLASH; break;
	case '%': t.kind = T_PERCENT; break;
	case '^': t.kind = T_CARET; break;
	case '(': t.kind = T_LPAREN; break;
	case ')': t.kind = T_RPAREN; break;
	case '[': t.kind = T_LBRACKET; break;
	case ']': t.kind = T_RBRACKET; break;
	case '{': t.kind = T_LBRACE; break;
	case '}': t.kind = T_RBRACE; break;
	case ',': t.kind = T_COMMA; break;
	case ':': t.kind = T_COLON; break;
	case '.': t.kind = T_DOT; break;
	case '|':
		if (*l->p == '>') { l->p++; t.kind = T_PIPE; t.len = 2; }
		else t.kind = T_ERROR;
		break;
	case '=':
		if (*l->p == '=') { l->p++; t.kind = T_EQ; t.len = 2; }
		else t.kind = T_ASSIGN;
		break;
	case '!':
		if (*l->p == '=') { l->p++; t.kind = T_NEQ; t.len = 2; }
		else t.kind = T_ERROR;
		break;
	case '<':
		if (*l->p == '=') { l->p++; t.kind = T_LE; t.len = 2; }
		else t.kind = T_LT;
		break;
	case '>':
		if (*l->p == '=') { l->p++; t.kind = T_GE; t.len = 2; }
		else t.kind = T_GT;
		break;
	default: t.kind = T_ERROR; break;
	}
	return t;
}

/* -------------------------------------------------------------- compiler */

struct local {
	const char *name;
	u32         len;
	u8          depth;
	bool        captured;
};

struct loop {
	u32 start;              /* where `skip` jumps back to */
	u32 breaks[16];         /* patched to the loop exit */
	u8  nbreaks;
};

struct compiler {
	struct she_vm *vm;
	struct lexer   lex;
	struct token   cur, prev;
	const char    *origin;

	struct she_obj   *fn;
	struct she_chunk *chunk;

	struct local locals[MAX_LOCALS];
	u8  nlocals;
	u8  scope_depth;
	u8  nest;

	struct loop loops[8];
	u8  nloops;

	struct compiler *enclosing;
	bool panic;
};

static void advance(struct compiler *c)
{
	c->prev = c->cur;
	c->cur = lex_next(&c->lex);
}

/* Newlines are statement separators, but they must not break an expression
 * that is obviously continuing (a pipeline on the next line, for instance). */
static void skip_newlines(struct compiler *c)
{
	while (c->cur.kind == T_NEWLINE)
		advance(c);
}

static bool check(struct compiler *c, enum tok k) { return c->cur.kind == k; }

static bool match(struct compiler *c, enum tok k)
{
	if (!check(c, k))
		return false;
	advance(c);
	return true;
}

__printf(2, 3) static void cerror(struct compiler *c, const char *fmt, ...)
{
	if (c->panic)
		return;
	c->panic = true;
	c->vm->failed = true;
	c->vm->error_line = (int)c->cur.line;

	char detail[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);

	/* Quote what was actually found. "expected X" alone sends people back to
	 * the file to work out what the parser was looking at. */
	char found[40];
	u32 n = c->cur.len < sizeof(found) - 1 ? c->cur.len : (u32)sizeof(found) - 1;
	if (c->cur.kind == T_EOF) {
		strlcpy(found, "the end of the script", sizeof(found));
		/* Running out of input is not the same kind of error as writing
		 * something wrong: at a prompt it means the block is still being
		 * typed, and the shell should ask for the next line. */
		c->vm->incomplete = true;
	}
	else if (c->cur.kind == T_NEWLINE)
		strlcpy(found, "the end of the line", sizeof(found));
	else {
		memcpy(found, c->cur.start, n);
		found[n] = '\0';
	}

	snprintf(c->vm->error, sizeof(c->vm->error),
	         "%s, line %u: %s, but found \"%s\"",
	         c->origin ? c->origin : "script", c->cur.line, detail, found);
}

static void expect(struct compiler *c, enum tok k, const char *what)
{
	if (match(c, k))
		return;
	cerror(c, "expected %s", what);
}

static void emit(struct compiler *c, u8 byte)
{
	if (she_chunk_write(c->vm, c->chunk, byte, c->prev.line) != RK_OK)
		cerror(c, "the script is too large to compile");
}

static void emit2(struct compiler *c, u8 a, u8 b) { emit(c, a); emit(c, b); }

static u32 emit_jump(struct compiler *c, u8 op)
{
	emit(c, op);
	emit(c, 0xFF);
	emit(c, 0xFF);
	return c->chunk->len - 2;
}

static void patch_jump(struct compiler *c, u32 at)
{
	u32 distance = c->chunk->len - at - 2;
	if (distance > 0xFFFF) {
		cerror(c, "a branch in this script jumps further than the VM allows");
		return;
	}
	c->chunk->code[at]     = (u8)(distance & 0xFF);
	c->chunk->code[at + 1] = (u8)(distance >> 8);
}

static void emit_loop(struct compiler *c, u32 start)
{
	emit(c, OP_LOOP);
	u32 distance = c->chunk->len - start + 2;
	emit(c, (u8)(distance & 0xFF));
	emit(c, (u8)(distance >> 8));
}

static void emit_constant(struct compiler *c, struct she_value v)
{
	int idx = she_chunk_constant(c->vm, c->chunk, v);
	if (idx < 0 || idx > 255) {
		cerror(c, "this script uses more than 256 distinct literals");
		return;
	}
	emit2(c, OP_CONST, (u8)idx);
}

/* ------------------------------------------------------- string literals */

/* Unescape and expand {name} interpolation. Interpolation compiles to a
 * sequence of pushes and concatenations, which keeps the VM simple: it never
 * has to know that interpolation exists. */
static void compile_text(struct compiler *c)
{
	const char *s = c->prev.start;
	u32 len = c->prev.len;
	/* Initialised because the empty-string case passes it with a length of
	 * zero, which the compiler cannot see is safe. */
	char buf[256] = { 0 };
	u32 n = 0;
	u32 parts = 0;

	for (u32 i = 0; i < len; i++) {
		if (s[i] == '\\' && i + 1 < len) {
			i++;
			char e = s[i];
			char out = e;
			if (e == 'n') out = '\n';
			else if (e == 't') out = '\t';
			else if (e == 'r') out = '\r';
			else if (e == '0') out = '\0';
			if (n + 1 < sizeof(buf))
				buf[n++] = out;
			continue;
		}

		if (s[i] == '{') {
			u32 close = i + 1;
			while (close < len && s[close] != '}')
				close++;
			if (close >= len) {
				cerror(c, "an interpolation opened with { was never closed");
				return;
			}

			/* Flush the literal accumulated so far. */
			if (n) {
				emit_constant(c, she_obj_value(SHE_TEXT,
				              she_string_new(c->vm, buf, n)));
				if (parts++)
					emit(c, OP_CONCAT);
				n = 0;
			}

			/* Compile the embedded name as a variable read. */
			char name[64];
			u32 nl = close - i - 1;
			if (nl >= sizeof(name)) {
				cerror(c, "the name inside {} is too long");
				return;
			}
			memcpy(name, s + i + 1, nl);
			name[nl] = '\0';

			emit_constant(c, she_obj_value(SHE_TEXT,
			              she_string_new(c->vm, name, nl)));
			emit(c, OP_GET_GLOBAL);
			if (parts++)
				emit(c, OP_CONCAT);
			i = close;
			continue;
		}

		if (n + 1 < sizeof(buf))
			buf[n++] = s[i];
	}

	if (n || !parts) {
		emit_constant(c, she_obj_value(SHE_TEXT, she_string_new(c->vm, buf, n)));
		if (parts++)
			emit(c, OP_CONCAT);
	}
}

/* ---------------------------------------------------------------- scopes */

static void begin_scope(struct compiler *c) { c->scope_depth++; }

static void end_scope(struct compiler *c)
{
	c->scope_depth--;
	while (c->nlocals && c->locals[c->nlocals - 1].depth > c->scope_depth) {
		emit(c, OP_POP);
		c->nlocals--;
	}
}

static int resolve_local(struct compiler *c, const char *name, u32 len)
{
	for (int i = (int)c->nlocals - 1; i >= 0; i--)
		if (c->locals[i].len == len && strncmp(c->locals[i].name, name, len) == 0)
			return i;
	return -1;
}

static void add_local(struct compiler *c, const char *name, u32 len)
{
	if (c->nlocals >= MAX_LOCALS) {
		cerror(c, "too many names in one function (the limit is %u)", MAX_LOCALS);
		return;
	}
	c->locals[c->nlocals].name  = name;
	c->locals[c->nlocals].len   = len;
	c->locals[c->nlocals].depth = c->scope_depth;
	c->locals[c->nlocals].captured = false;
	c->nlocals++;
}

/* ------------------------------------------------------------ expressions */

static void expression(struct compiler *c);
static void statement(struct compiler *c);
static void declaration(struct compiler *c);

static u8 argument_list(struct compiler *c)
{
	u8 n = 0;
	if (!check(c, T_RPAREN)) {
		do {
			skip_newlines(c);
			expression(c);
			if (n == 255) {
				cerror(c, "a call may not have more than 255 arguments");
				return n;
			}
			n++;
			skip_newlines(c);
		} while (match(c, T_COMMA));
	}
	expect(c, T_RPAREN, "a closing parenthesis");
	return n;
}

static void lambda(struct compiler *c);

static void primary(struct compiler *c)
{
	if (c->nest++ > MAX_NEST) {
		cerror(c, "this expression nests deeper than %u levels", MAX_NEST);
		c->nest--;
		return;
	}

	if (match(c, T_NUMBER)) {
		emit_constant(c, SHE_NUM_V(c->prev.number));
	} else if (match(c, T_REAL)) {
		struct she_value v = { .type = SHE_REAL };
		v.as.r = c->prev.real;
		emit_constant(c, v);
	} else if (match(c, T_TEXT)) {
		compile_text(c);
	} else if (match(c, T_TRUE)) {
		emit(c, OP_TRUE);
	} else if (match(c, T_FALSE)) {
		emit(c, OP_FALSE);
	} else if (match(c, T_NOTHING)) {
		emit(c, OP_NOTHING);
	} else if (match(c, T_ASK)) {
		/* `ask "prompt"` reads a line. The prompt is optional. */
		if (check(c, T_TEXT) || check(c, T_IDENT))
			expression(c);
		else
			emit(c, OP_NOTHING);
		emit(c, OP_ASK);
	} else if (match(c, T_FUN)) {
		lambda(c);
	} else if (match(c, T_LPAREN)) {
		skip_newlines(c);
		expression(c);
		skip_newlines(c);
		expect(c, T_RPAREN, "a closing parenthesis");
	} else if (match(c, T_LBRACKET)) {
		u8 n = 0;
		skip_newlines(c);
		if (!check(c, T_RBRACKET)) {
			do {
				skip_newlines(c);
				expression(c);
				n++;
				skip_newlines(c);
			} while (match(c, T_COMMA));
		}
		expect(c, T_RBRACKET, "a closing bracket");
		emit2(c, OP_MAKE_LIST, n);
	} else if (match(c, T_LBRACE)) {
		u8 n = 0;
		skip_newlines(c);
		if (!check(c, T_RBRACE)) {
			do {
				skip_newlines(c);
				if (match(c, T_IDENT) || match(c, T_TEXT)) {
					emit_constant(c, she_obj_value(SHE_TEXT,
					              she_string_new(c->vm, c->prev.start, c->prev.len)));
				} else {
					cerror(c, "a map key must be a name or text");
					return;
				}
				expect(c, T_COLON, "a colon after the key");
				expression(c);
				n++;
				skip_newlines(c);
			} while (match(c, T_COMMA));
		}
		expect(c, T_RBRACE, "a closing brace");
		emit2(c, OP_MAKE_MAP, n);
	} else if (match(c, T_IDENT)) {
		int slot = resolve_local(c, c->prev.start, c->prev.len);
		if (slot >= 0) {
			emit2(c, OP_GET_LOCAL, (u8)slot);
		} else {
			emit_constant(c, she_obj_value(SHE_TEXT,
			              she_string_new(c->vm, c->prev.start, c->prev.len)));
			emit(c, OP_GET_GLOBAL);
		}
	} else {
		cerror(c, "expected a value");
	}
	c->nest--;
}

static void postfix(struct compiler *c)
{
	primary(c);
	for (;;) {
		if (match(c, T_LPAREN)) {
			u8 n = argument_list(c);
			emit2(c, OP_CALL, n);
		} else if (match(c, T_LBRACKET)) {
			expression(c);
			expect(c, T_RBRACKET, "a closing bracket");
			emit(c, OP_GET_INDEX);
		} else if (match(c, T_DOT)) {
			expect(c, T_IDENT, "a field name after the dot");
			emit_constant(c, she_obj_value(SHE_TEXT,
			              she_string_new(c->vm, c->prev.start, c->prev.len)));
			emit(c, OP_GET_FIELD);
		} else {
			return;
		}
	}
}

static void unary(struct compiler *c)
{
	if (match(c, T_MINUS)) {
		unary(c);
		emit(c, OP_NEG);
	} else if (match(c, T_NOT)) {
		unary(c);
		emit(c, OP_NOT);
	} else {
		postfix(c);
	}
}

static void factor(struct compiler *c)
{
	unary(c);
	for (;;) {
		if (match(c, T_STAR))         { unary(c); emit(c, OP_MUL); }
		else if (match(c, T_SLASH))   { unary(c); emit(c, OP_DIV); }
		else if (match(c, T_PERCENT)) { unary(c); emit(c, OP_MOD); }
		else if (match(c, T_CARET))   { unary(c); emit(c, OP_POW); }
		else return;
	}
}

static void term(struct compiler *c)
{
	factor(c);
	for (;;) {
		if (match(c, T_PLUS))       { factor(c); emit(c, OP_ADD); }
		else if (match(c, T_MINUS)) { factor(c); emit(c, OP_SUB); }
		else return;
	}
}

static void range_expr(struct compiler *c)
{
	term(c);
	if (match(c, T_TO)) {
		term(c);
		emit(c, OP_MAKE_RANGE);
	}
}

static void comparison(struct compiler *c)
{
	range_expr(c);
	for (;;) {
		if (match(c, T_LT))      { range_expr(c); emit(c, OP_LT); }
		else if (match(c, T_LE)) { range_expr(c); emit(c, OP_LE); }
		else if (match(c, T_GT)) { range_expr(c); emit(c, OP_GT); }
		else if (match(c, T_GE)) { range_expr(c); emit(c, OP_GE); }
		else return;
	}
}

static void equality(struct compiler *c)
{
	comparison(c);
	for (;;) {
		if (match(c, T_IS)) {
			/* `is not` is the negated form, and reads the way people say it. */
			bool negate = match(c, T_NOT);
			comparison(c);
			emit(c, negate ? OP_NEQ : OP_EQ);
		} else if (match(c, T_EQ)) {
			comparison(c);
			emit(c, OP_EQ);
		} else if (match(c, T_NEQ)) {
			comparison(c);
			emit(c, OP_NEQ);
		} else {
			return;
		}
	}
}

static void and_expr(struct compiler *c)
{
	equality(c);
	while (match(c, T_AND)) {
		/* Short circuit: leave the left value if it is false. */
		u32 skip = emit_jump(c, OP_JUMP_IF_FALSE);
		emit(c, OP_POP);
		equality(c);
		patch_jump(c, skip);
	}
}

static void or_expr(struct compiler *c)
{
	and_expr(c);
	while (match(c, T_OR)) {
		u32 skip = emit_jump(c, OP_JUMP_IF_TRUE);
		emit(c, OP_POP);
		and_expr(c);
		patch_jump(c, skip);
	}
}

/* The pipeline operator. `x |> f(a)` compiles to a call of f with x inserted
 * as the first argument, which is why it reads as a chain rather than as
 * nesting turned inside out. */
static void pipeline(struct compiler *c)
{
	or_expr(c);
	while (match(c, T_PIPE)) {
		skip_newlines(c);
		if (!match(c, T_IDENT)) {
			cerror(c, "expected a function name after |>");
			return;
		}
		int slot = resolve_local(c, c->prev.start, c->prev.len);
		if (slot >= 0)
			emit2(c, OP_GET_LOCAL, (u8)slot);
		else {
			emit_constant(c, she_obj_value(SHE_TEXT,
			              she_string_new(c->vm, c->prev.start, c->prev.len)));
			emit(c, OP_GET_GLOBAL);
		}

		u8 extra = 0;
		if (match(c, T_LPAREN))
			extra = argument_list(c);

		/* Stack is now [value, callee, args...]; OP_PIPE rotates the value
		 * into the first argument position and performs the call. */
		emit2(c, OP_PIPE, extra);
	}
}

static void expression(struct compiler *c) { pipeline(c); }

/* ------------------------------------------------------------- functions */

static void function_body(struct compiler *c, struct compiler *sub, bool arrow)
{
	sub->vm        = c->vm;
	sub->lex       = c->lex;
	sub->cur       = c->cur;
	sub->prev      = c->prev;
	sub->origin    = c->origin;
	sub->enclosing = c;
	sub->chunk     = she_function_chunk(sub->fn);
	sub->nest      = c->nest;

	/* Slot 0 of a frame holds the callee itself, so reserve it before any
	 * parameter can claim it. */
	add_local(sub, "", 0);
	begin_scope(sub);

	expect(sub, T_LPAREN, "a parenthesis after fun");
	u8 arity = 0;
	if (!check(sub, T_RPAREN)) {
		do {
			expect(sub, T_IDENT, "a parameter name");
			add_local(sub, sub->prev.start, sub->prev.len);
			arity++;
		} while (match(sub, T_COMMA));
	}
	expect(sub, T_RPAREN, "a closing parenthesis");

	if (arrow) {
		expect(sub, T_ARROW, "-> before the body of a short function");
		expression(sub);
		emit(sub, OP_RETURN);
	} else {
		skip_newlines(sub);
		while (!check(sub, T_END) && !check(sub, T_EOF)) {
			declaration(sub);
			skip_newlines(sub);
		}
		expect(sub, T_END, "end to close the function");
		emit(sub, OP_NOTHING);
		emit(sub, OP_RETURN);
	}

	/* Hand the cursor back to the enclosing compiler. */
	c->lex  = sub->lex;
	c->cur  = sub->cur;
	c->prev = sub->prev;
	if (sub->vm->failed)
		c->panic = true;

	she_function_set_arity(sub->fn, arity);
}

static void lambda(struct compiler *c)
{
	/* `fun name(...)` declares; `fun(...) ->` is an expression. */
	struct compiler sub;
	memset(&sub, 0, sizeof(sub));
	sub.fn = she_function_new(c->vm, "lambda", 0);
	if (!sub.fn) {
		cerror(c, "out of memory while compiling a function");
		return;
	}
	function_body(c, &sub, true);
	emit_constant(c, she_obj_value(SHE_FUNCTION, sub.fn));
}

/* ------------------------------------------------------------- statements */

static void block_until_end(struct compiler *c, const char *what)
{
	begin_scope(c);
	skip_newlines(c);
	while (!check(c, T_END) && !check(c, T_ELSE) && !check(c, T_EOF)) {
		declaration(c);
		skip_newlines(c);
	}
	end_scope(c);
	(void)what;
}

static void if_statement(struct compiler *c)
{
	expression(c);
	match(c, T_THEN);

	u32 else_jump = emit_jump(c, OP_JUMP_IF_FALSE);
	emit(c, OP_POP);
	block_until_end(c, "if");

	u32 end_jump = emit_jump(c, OP_JUMP);
	patch_jump(c, else_jump);
	emit(c, OP_POP);

	if (match(c, T_ELSE)) {
		if (match(c, T_IF)) {
			if_statement(c);        /* else if chains without extra ends */
			patch_jump(c, end_jump);
			return;
		}
		block_until_end(c, "else");
	}
	expect(c, T_END, "end to close the if");
	patch_jump(c, end_jump);
}

static void push_loop(struct compiler *c, u32 start)
{
	if (c->nloops >= ARRAY_SIZE(c->loops)) {
		cerror(c, "loops are nested deeper than the compiler supports");
		return;
	}
	c->loops[c->nloops].start = start;
	c->loops[c->nloops].nbreaks = 0;
	c->nloops++;
}

static void pop_loop(struct compiler *c)
{
	if (!c->nloops)
		return;
	struct loop *l = &c->loops[--c->nloops];
	for (u8 i = 0; i < l->nbreaks; i++)
		patch_jump(c, l->breaks[i]);
}

static void while_statement(struct compiler *c)
{
	u32 start = c->chunk->len;
	push_loop(c, start);

	expression(c);
	u32 exit = emit_jump(c, OP_JUMP_IF_FALSE);
	emit(c, OP_POP);

	block_until_end(c, "while");
	expect(c, T_END, "end to close the while");

	emit_loop(c, start);
	patch_jump(c, exit);
	emit(c, OP_POP);
	pop_loop(c);
}

static void repeat_statement(struct compiler *c)
{
	u32 start = c->chunk->len;
	push_loop(c, start);

	begin_scope(c);
	skip_newlines(c);
	while (!check(c, T_UNTIL) && !check(c, T_EOF)) {
		declaration(c);
		skip_newlines(c);
	}
	end_scope(c);
	expect(c, T_UNTIL, "until with the condition that stops the loop");

	expression(c);
	u32 exit = emit_jump(c, OP_JUMP_IF_TRUE);
	emit(c, OP_POP);
	emit_loop(c, start);
	patch_jump(c, exit);
	emit(c, OP_POP);
	pop_loop(c);
}

/* `for each x in collection ... end`. The iterator state lives in two hidden
 * locals, so the loop needs no VM-side iterator object. */
static void foreach_statement(struct compiler *c)
{
	expect(c, T_EACH, "each after for");
	expect(c, T_IDENT, "the name each item should be called");
	struct token var = c->prev;
	expect(c, T_IN, "in, followed by what to loop over");

	begin_scope(c);
	expression(c);
	u8 iter_slot = c->nlocals;
	add_local(c, "  iterable", 10);
	emit_constant(c, SHE_NUM_V(0));
	add_local(c, "  index", 7);

	u32 start = c->chunk->len;
	push_loop(c, start);

	/* Reads the hidden iterable and cursor at iter_slot and iter_slot+1. On
	 * each step it pushes the item then yes; when exhausted it pushes no. */
	emit2(c, OP_ITER_NEXT, iter_slot);
	u32 exit = emit_jump(c, OP_JUMP_IF_FALSE);
	emit(c, OP_POP);

	begin_scope(c);
	add_local(c, var.start, var.len);
	skip_newlines(c);
	while (!check(c, T_END) && !check(c, T_EOF)) {
		declaration(c);
		skip_newlines(c);
	}
	end_scope(c);
	expect(c, T_END, "end to close the loop");

	emit_loop(c, start);
	patch_jump(c, exit);
	emit(c, OP_POP);
	pop_loop(c);
	end_scope(c);
}

static void say_statement(struct compiler *c)
{
	expression(c);
	emit(c, OP_SAY);
}

static void let_statement(struct compiler *c)
{
	expect(c, T_IDENT, "a name after let");
	struct token name = c->prev;

	if (match(c, T_ASSIGN))
		expression(c);
	else
		emit(c, OP_NOTHING);

	if (c->scope_depth > 0) {
		add_local(c, name.start, name.len);
		/* The value is already on the stack in the local's slot. */
	} else {
		emit_constant(c, she_obj_value(SHE_TEXT,
		              she_string_new(c->vm, name.start, name.len)));
		emit(c, OP_DEF_GLOBAL);
	}
}

static void fun_declaration(struct compiler *c)
{
	expect(c, T_IDENT, "a name for the function");
	struct token name = c->prev;

	struct compiler sub;
	memset(&sub, 0, sizeof(sub));
	char fname[32];
	u32 n = name.len < sizeof(fname) - 1 ? name.len : (u32)sizeof(fname) - 1;
	memcpy(fname, name.start, n);
	fname[n] = '\0';

	sub.fn = she_function_new(c->vm, fname, 0);
	if (!sub.fn) {
		cerror(c, "out of memory while compiling %s", fname);
		return;
	}
	function_body(c, &sub, false);

	emit_constant(c, she_obj_value(SHE_FUNCTION, sub.fn));
	emit_constant(c, she_obj_value(SHE_TEXT, she_string_new(c->vm, name.start, name.len)));
	emit(c, OP_DEF_GLOBAL);
}

static void assignment_or_expression(struct compiler *c)
{
	/* Look ahead for `name = value` without a full backtracking parser: an
	 * identifier followed by = at statement position is an assignment. */
	if (check(c, T_IDENT)) {
		struct lexer save_lex = c->lex;
		struct token save_cur = c->cur, save_prev = c->prev;

		advance(c);
		struct token name = c->prev;
		if (check(c, T_ASSIGN)) {
			advance(c);
			expression(c);
			int slot = resolve_local(c, name.start, name.len);
			if (slot >= 0) {
				emit2(c, OP_SET_LOCAL, (u8)slot);
			} else {
				emit_constant(c, she_obj_value(SHE_TEXT,
				              she_string_new(c->vm, name.start, name.len)));
				emit(c, OP_SET_GLOBAL);
			}
			emit(c, OP_POP);
			return;
		}
		c->lex = save_lex;
		c->cur = save_cur;
		c->prev = save_prev;
	}

	expression(c);
	emit(c, OP_POP);
}

static void statement(struct compiler *c)
{
	if (match(c, T_SAY))          say_statement(c);
	else if (match(c, T_IF))      if_statement(c);
	else if (match(c, T_WHILE))   while_statement(c);
	else if (match(c, T_REPEAT))  repeat_statement(c);
	else if (match(c, T_FOR))     foreach_statement(c);
	else if (match(c, T_RETURN)) {
		if (check(c, T_NEWLINE) || check(c, T_END) || check(c, T_EOF))
			emit(c, OP_NOTHING);
		else
			expression(c);
		emit(c, OP_RETURN);
	} else if (match(c, T_BREAK)) {
		if (!c->nloops) {
			cerror(c, "break is only meaningful inside a loop");
			return;
		}
		struct loop *l = &c->loops[c->nloops - 1];
		if (l->nbreaks < ARRAY_SIZE(l->breaks))
			l->breaks[l->nbreaks++] = emit_jump(c, OP_JUMP);
	} else if (match(c, T_SKIP)) {
		if (!c->nloops) {
			cerror(c, "skip is only meaningful inside a loop");
			return;
		}
		emit_loop(c, c->loops[c->nloops - 1].start);
	} else {
		assignment_or_expression(c);
	}
}

static void declaration(struct compiler *c)
{
	if (match(c, T_LET))
		let_statement(c);
	else if (check(c, T_FUN) && c->scope_depth == 0) {
		advance(c);
		fun_declaration(c);
	} else
		statement(c);

	/* Recover at the next line so one syntax error does not cascade. */
	if (c->panic) {
		c->panic = false;
		while (!check(c, T_NEWLINE) && !check(c, T_EOF))
			advance(c);
	}
}

/* ------------------------------------------------------------------- API */

int she_compile(struct she_vm *vm, const char *source, const char *origin,
                struct she_obj **out_fn)
{
	struct compiler c;
	memset(&c, 0, sizeof(c));

	c.vm  = vm;
	c.origin = origin;
	c.lex.src = source;
	c.lex.p = source;
	c.lex.line = 1;

	c.fn = she_function_new(vm, origin ? origin : "script", 0);
	if (!c.fn)
		return RK_ENOMEM;
	c.chunk = she_function_chunk(c.fn);
	add_local(&c, "", 0);   /* the script itself occupies frame slot 0 */

	vm->failed = false;
	vm->incomplete = false;
	vm->error[0] = '\0';

	advance(&c);
	skip_newlines(&c);
	while (!check(&c, T_EOF)) {
		declaration(&c);
		skip_newlines(&c);
	}

	emit(&c, OP_NOTHING);
	emit(&c, OP_RETURN);

	if (vm->failed)
		return RK_EINVAL;

	*out_fn = c.fn;
	return RK_OK;
}
