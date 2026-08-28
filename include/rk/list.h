/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - intrusive circular doubly-linked list.
 *
 * Intrusive because the kernel must link objects onto lists in contexts where
 * allocation is forbidden: interrupt handlers, the allocator, the scheduler.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void list_init(struct list_head *l) { l->next = l->prev = l; }
static inline bool list_empty(const struct list_head *l) { return l->next == l; }

static inline void __list_add(struct list_head *n, struct list_head *prev, struct list_head *next)
{
	next->prev = n;
	n->next = next;
	n->prev = prev;
	prev->next = n;
}

static inline void list_add(struct list_head *n, struct list_head *head)      { __list_add(n, head, head->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *head) { __list_add(n, head->prev, head); }

/* Leaves the node self-linked, so a second list_del is a harmless no-op. */
static inline void list_del(struct list_head *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->next = n->prev = n;
}

static inline bool list_linked(const struct list_head *n) { return n->next != n; }

static inline void list_move_tail(struct list_head *n, struct list_head *head)
{
	list_del(n);
	list_add_tail(n, head);
}

static inline size_t list_count(const struct list_head *head)
{
	size_t n = 0;
	for (const struct list_head *p = head->next; p != head; p = p->next)
		n++;
	return n;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(head, type, member) list_entry((head)->next, type, member)
#define list_last_entry(head, type, member)  list_entry((head)->prev, type, member)

#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, tmp, head) \
	for (pos = (head)->next, tmp = pos->next; pos != (head); pos = tmp, tmp = pos->next)

#define list_for_each_entry(pos, head, member)                        \
	for (pos = list_entry((head)->next, __typeof__(*pos), member);     \
	     &pos->member != (head);                                       \
	     pos = list_entry(pos->member.next, __typeof__(*pos), member))

#define list_for_each_entry_safe(pos, tmp, head, member)              \
	for (pos = list_entry((head)->next, __typeof__(*pos), member),     \
	     tmp = list_entry(pos->member.next, __typeof__(*pos), member); \
	     &pos->member != (head);                                       \
	     pos = tmp, tmp = list_entry(tmp->member.next, __typeof__(*tmp), member))
