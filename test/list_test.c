/*
 * test/list_test.c — unit tests for src/lib/list.c
 *
 * Covers: list_init, list_is_empty, list_insert_head, list_insert_tail,
 *         list_remove_head, list_remove_tail, list_remove_entry,
 *         offset_of / container_of macros.
 *
 * All tests use stack-allocated list_entry nodes — no heap or phys memory.
 */

#include <lib/list.h>
#include <test/test.h>

/* ── list_init / list_is_empty ───────────────────────────────────── */

KTEST(list, init_empty)
{
	list_entry head;
	list_init(&head);
	EXPECT_TRUE(list_is_empty(&head));
	return 0;
}

/* ── list_insert_head ────────────────────────────────────────────── */

KTEST(list, insert_head_single)
{
	list_entry head, a;
	list_init(&head);
	list_insert_head(&head, &a);
	EXPECT_FALSE(list_is_empty(&head));
	EXPECT_EQ(head.next, &a);
	EXPECT_EQ(head.prev, &a);
	EXPECT_EQ(a.prev, &head);
	EXPECT_EQ(a.next, &head);
	return 0;
}

KTEST(list, insert_head_order)
{
	/* Inserting A then B via head: head → B → A → head */
	list_entry head, a, b;
	list_init(&head);
	list_insert_head(&head, &a);
	list_insert_head(&head, &b);

	EXPECT_EQ(head.next, &b);
	EXPECT_EQ(b.next, &a);
	EXPECT_EQ(a.next, &head);
	return 0;
}

KTEST(list, insert_head_remove_head_lifo)
{
	/* insert A, B, C via head → remove head yields C, B, A (LIFO) */
	list_entry head, a, b, c;
	list_init(&head);
	list_insert_head(&head, &a);
	list_insert_head(&head, &b);
	list_insert_head(&head, &c);

	EXPECT_EQ(list_remove_head(&head), &c);
	EXPECT_EQ(list_remove_head(&head), &b);
	EXPECT_EQ(list_remove_head(&head), &a);
	EXPECT_TRUE(list_is_empty(&head));
	return 0;
}

/* ── list_insert_tail ────────────────────────────────────────────── */

KTEST(list, insert_tail_single)
{
	list_entry head, a;
	list_init(&head);
	list_insert_tail(&head, &a);
	EXPECT_FALSE(list_is_empty(&head));
	EXPECT_EQ(head.prev, &a);
	EXPECT_EQ(head.next, &a);
	return 0;
}

KTEST(list, insert_tail_order)
{
	/* Inserting A then B via tail: head → A → B → head */
	list_entry head, a, b;
	list_init(&head);
	list_insert_tail(&head, &a);
	list_insert_tail(&head, &b);

	EXPECT_EQ(head.next, &a);
	EXPECT_EQ(a.next, &b);
	EXPECT_EQ(b.next, &head);
	return 0;
}

KTEST(list, insert_tail_remove_head_fifo)
{
	/* insert A, B, C via tail → remove_head yields A, B, C (FIFO) */
	list_entry head, a, b, c;
	list_init(&head);
	list_insert_tail(&head, &a);
	list_insert_tail(&head, &b);
	list_insert_tail(&head, &c);

	EXPECT_EQ(list_remove_head(&head), &a);
	EXPECT_EQ(list_remove_head(&head), &b);
	EXPECT_EQ(list_remove_head(&head), &c);
	EXPECT_TRUE(list_is_empty(&head));
	return 0;
}

KTEST(list, insert_tail_remove_tail_lifo)
{
	/* insert A, B, C via tail → remove_tail yields C, B, A */
	list_entry head, a, b, c;
	list_init(&head);
	list_insert_tail(&head, &a);
	list_insert_tail(&head, &b);
	list_insert_tail(&head, &c);

	EXPECT_EQ(list_remove_tail(&head), &c);
	EXPECT_EQ(list_remove_tail(&head), &b);
	EXPECT_EQ(list_remove_tail(&head), &a);
	EXPECT_TRUE(list_is_empty(&head));
	return 0;
}

/* ── list_remove_entry ───────────────────────────────────────────── */

KTEST(list, remove_entry_middle)
{
	/* Remove B from [A, B, C] → [A, C] */
	list_entry head, a, b, c;
	list_init(&head);
	list_insert_tail(&head, &a);
	list_insert_tail(&head, &b);
	list_insert_tail(&head, &c);

	list_remove_entry(&b);

	/* list is now [A, C] */
	EXPECT_EQ(head.next, &a);
	EXPECT_EQ(a.next, &c);
	EXPECT_EQ(c.next, &head);
	EXPECT_FALSE(list_is_empty(&head));
	return 0;
}

KTEST(list, remove_entry_head)
{
	list_entry head, a, b;
	list_init(&head);
	list_insert_tail(&head, &a);
	list_insert_tail(&head, &b);

	list_remove_entry(&a);

	EXPECT_EQ(head.next, &b);
	EXPECT_EQ(b.next, &head);
	return 0;
}

KTEST(list, remove_entry_tail_node)
{
	list_entry head, a, b;
	list_init(&head);
	list_insert_tail(&head, &a);
	list_insert_tail(&head, &b);

	list_remove_entry(&b);

	EXPECT_EQ(head.next, &a);
	EXPECT_EQ(a.next, &head);
	return 0;
}

KTEST(list, remove_entry_returns_empty)
{
	/* remove_entry of the only element: return value must be non-zero
	 * (list_remove_entry returns 1 when the list becomes empty) */
	list_entry head, a;
	list_init(&head);
	list_insert_tail(&head, &a);

	int became_empty = list_remove_entry(&a);
	EXPECT_TRUE(became_empty);
	EXPECT_TRUE(list_is_empty(&head));
	return 0;
}

KTEST(list, remove_entry_returns_not_empty)
{
	/* removing one of two elements: return value must be 0 */
	list_entry head, a, b;
	list_init(&head);
	list_insert_tail(&head, &a);
	list_insert_tail(&head, &b);

	int became_empty = list_remove_entry(&a);
	EXPECT_FALSE(became_empty);
	return 0;
}

/* ── is_empty after full drain ───────────────────────────────────── */

KTEST(list, empty_after_drain)
{
	list_entry head, a, b, c;
	list_init(&head);
	list_insert_tail(&head, &a);
	list_insert_tail(&head, &b);
	list_insert_tail(&head, &c);

	list_remove_head(&head);
	EXPECT_FALSE(list_is_empty(&head));
	list_remove_head(&head);
	EXPECT_FALSE(list_is_empty(&head));
	list_remove_head(&head);
	EXPECT_TRUE(list_is_empty(&head));
	return 0;
}

/* ── container_of / offset_of ────────────────────────────────────── */

typedef struct {
	int value;
	list_entry node;
} test_item_t;

KTEST(list, container_of)
{
	test_item_t item;
	item.value = 42;

	/* Recover the enclosing struct from a pointer to the embedded node. */
	test_item_t *recovered = container_of(&item.node, test_item_t, node);

	EXPECT_EQ(recovered, &item);
	EXPECT_EQ(recovered->value, 42);
	return 0;
}

KTEST(list, offset_of_nonzero_for_second_field)
{
	/* offset_of node inside test_item_t must equal sizeof(int) */
	unsigned off = (unsigned)offset_of(test_item_t, node);
	EXPECT_GE((int)off, (int)sizeof(int));
	return 0;
}
