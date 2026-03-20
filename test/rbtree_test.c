/*
 * test/rbtree_test.c — unit tests for the hash_table API (lib/rbtree.c).
 *
 * Uses small integer values cast to void * as keys so that the default
 * pointer-difference comparator gives a stable, predictable order.
 * The test suite name is HashTest to reflect the public API being tested.
 */

#include <lib/rbtree.h>
#include <lib/klib.h>
#include <test/test.h>

/* Convenience: integer n → opaque key pointer */
#define K(n) ((void *)(long)(n))
/* Convenience: opaque value pointer → integer */
#define V(p) ((long)(p))

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Simple string comparator for custom-comparator tests. */
static int str_comp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

/* Evict-counter support — reset before each test that needs it. */
static int g_evict_count = 0;

static void counting_evict(const key_value_pair *pair)
{
	g_evict_count++;
}

/* ── hash_create / hash_destroy ──────────────────────────────────────────── */

KTEST(hash, create_destroy)
{
	hash_table *t = hash_create(NULL, NULL);
	ASSERT_NONNULL(t);
	EXPECT_TRUE(hash_isempty(t));
	EXPECT_EQ((int)hash_size(t), 0);
	hash_destroy(t);
	return 0;
}

KTEST(hash, create_null_is_empty)
{
	EXPECT_TRUE(hash_isempty(NULL));
	return 0;
}

/* ── hash_insert / hash_find ─────────────────────────────────────────────── */

KTEST(hash, insert_find)
{
	hash_table *t = hash_create(NULL, NULL);
	ASSERT_NONNULL(t);

	int ret = hash_insert(t, K(42), K(99));
	EXPECT_EQ(ret, 1);

	key_value_pair *p = hash_find(t, K(42));
	ASSERT_NONNULL(p);
	EXPECT_EQ(V(p->val), 99);

	hash_destroy(t);
	return 0;
}

KTEST(hash, find_missing)
{
	hash_table *t = hash_create(NULL, NULL);
	ASSERT_NONNULL(t);

	EXPECT_NULL(hash_find(t, K(1)));

	hash_destroy(t);
	return 0;
}

KTEST(hash, insert_multiple)
{
	hash_table *t = hash_create(NULL, NULL);
	int i;
	for (i = 1; i <= 5; i++)
		hash_insert(t, K(i), K(i * 10));

	for (i = 1; i <= 5; i++) {
		key_value_pair *p = hash_find(t, K(i));
		ASSERT_NONNULL(p);
		EXPECT_EQ(V(p->val), i * 10);
	}

	hash_destroy(t);
	return 0;
}

KTEST(hash, insert_duplicate_ignored)
{
	hash_table *t = hash_create(NULL, NULL);

	hash_insert(t, K(7), K(10));
	int ret = hash_insert(t, K(7), K(20)); /* duplicate key */
	EXPECT_EQ(ret, 0); /* rejected */

	key_value_pair *p = hash_find(t, K(7));
	ASSERT_NONNULL(p);
	EXPECT_EQ(V(p->val), 10); /* original value preserved */

	EXPECT_EQ((int)hash_size(t), 1);

	hash_destroy(t);
	return 0;
}

/* ── hash_size / hash_isempty ────────────────────────────────────────────── */

KTEST(hash, size_tracking)
{
	hash_table *t = hash_create(NULL, NULL);

	EXPECT_EQ((int)hash_size(t), 0);
	hash_insert(t, K(1), K(0));
	EXPECT_EQ((int)hash_size(t), 1);
	hash_insert(t, K(2), K(0));
	EXPECT_EQ((int)hash_size(t), 2);
	hash_remove(t, K(1));
	EXPECT_EQ((int)hash_size(t), 1);

	hash_destroy(t);
	return 0;
}

KTEST(hash, is_empty)
{
	hash_table *t = hash_create(NULL, NULL);

	EXPECT_TRUE(hash_isempty(t));
	hash_insert(t, K(1), K(0));
	EXPECT_FALSE(hash_isempty(t));
	hash_remove(t, K(1));
	EXPECT_TRUE(hash_isempty(t));

	hash_destroy(t);
	return 0;
}

/* ── hash_remove ─────────────────────────────────────────────────────────── */

KTEST(hash, remove)
{
	hash_table *t = hash_create(NULL, NULL);

	hash_insert(t, K(5), K(50));
	int ret = hash_remove(t, K(5));
	EXPECT_EQ(ret, 1);
	EXPECT_NULL(hash_find(t, K(5)));
	EXPECT_EQ((int)hash_size(t), 0);

	hash_destroy(t);
	return 0;
}

KTEST(hash, remove_nonexistent)
{
	hash_table *t = hash_create(NULL, NULL);

	int ret = hash_remove(t, K(99));
	EXPECT_EQ(ret, 0);

	hash_destroy(t);
	return 0;
}

KTEST(hash, remove_middle)
{
	hash_table *t = hash_create(NULL, NULL);
	int i;

	for (i = 1; i <= 5; i++)
		hash_insert(t, K(i), K(i));

	hash_remove(t, K(3));
	EXPECT_EQ((int)hash_size(t), 4);
	EXPECT_NULL(hash_find(t, K(3)));

	/* Remaining keys still findable */
	for (i = 1; i <= 5; i++) {
		if (i == 3)
			continue;
		EXPECT_NONNULL(hash_find(t, K(i)));
	}

	hash_destroy(t);
	return 0;
}

/* ── hash_remove_at ──────────────────────────────────────────────────────── */

KTEST(hash, remove_at)
{
	hash_table *t = hash_create(NULL, NULL);

	hash_insert(t, K(10), K(100));
	key_value_pair *p = hash_find(t, K(10));
	ASSERT_NONNULL(p);

	int ret = hash_remove_at(t, p); /* p is freed inside */
	EXPECT_EQ(ret, 1);
	EXPECT_NULL(hash_find(t, K(10)));
	EXPECT_EQ((int)hash_size(t), 0);

	hash_destroy(t);
	return 0;
}

KTEST(hash, remove_at_null)
{
	hash_table *t = hash_create(NULL, NULL);

	EXPECT_EQ(hash_remove_at(t, NULL), 0);

	hash_destroy(t);
	return 0;
}

/* ── hash_update ─────────────────────────────────────────────────────────── */

KTEST(hash, update)
{
	hash_table *t = hash_create(NULL, NULL);

	hash_insert(t, K(3), K(30));
	int ret = hash_update(t, K(3), K(300));
	EXPECT_EQ(ret, 1);

	key_value_pair *p = hash_find(t, K(3));
	ASSERT_NONNULL(p);
	EXPECT_EQ(V(p->val), 300);

	hash_destroy(t);
	return 0;
}

KTEST(hash, update_nonexistent)
{
	hash_table *t = hash_create(NULL, NULL);

	int ret = hash_update(t, K(99), K(0));
	EXPECT_EQ(ret, 0);

	hash_destroy(t);
	return 0;
}

/* ── hash_first / hash_next (iteration) ─────────────────────────────────── */

KTEST(hash, iteration_empty)
{
	hash_table *t = hash_create(NULL, NULL);

	EXPECT_NULL(hash_first(t));

	hash_destroy(t);
	return 0;
}

KTEST(hash, iteration_single)
{
	hash_table *t = hash_create(NULL, NULL);

	hash_insert(t, K(7), K(70));
	key_value_pair *p = hash_first(t);
	ASSERT_NONNULL(p);
	EXPECT_EQ(V(p->key), 7);
	EXPECT_NULL(hash_next(t, p));

	hash_destroy(t);
	return 0;
}

KTEST(hash, iteration_visits_all)
{
	hash_table *t = hash_create(NULL, NULL);
	int i, count = 0;
	int seen[6] = { 0 };

	for (i = 1; i <= 5; i++)
		hash_insert(t, K(i), K(i));

	key_value_pair *p;
	for (p = hash_first(t); p; p = hash_next(t, p)) {
		long k = V(p->key);
		EXPECT_GE((int)k, 1);
		EXPECT_LE((int)k, 5);
		seen[k]++;
		count++;
	}

	EXPECT_EQ(count, 5);
	for (i = 1; i <= 5; i++)
		EXPECT_EQ(seen[i], 1);

	hash_destroy(t);
	return 0;
}

KTEST(hash, iteration_sorted_order)
{
	/* With integer keys and the default comparator, in-order traversal
	 * must yield keys in ascending numeric order. */
	hash_table *t = hash_create(NULL, NULL);
	int keys[] = { 5, 2, 8, 1, 4 };
	int i;

	for (i = 0; i < 5; i++)
		hash_insert(t, K(keys[i]), K(0));

	long prev = -1;
	key_value_pair *p;
	for (p = hash_first(t); p; p = hash_next(t, p)) {
		long k = V(p->key);
		EXPECT_GT((int)k, (int)prev);
		prev = k;
	}

	hash_destroy(t);
	return 0;
}

/* ── evict callback ──────────────────────────────────────────────────────── */

KTEST(hash, evict_on_remove)
{
	hash_table *t = hash_create(NULL, counting_evict);
	g_evict_count = 0;

	hash_insert(t, K(1), K(0));
	hash_insert(t, K(2), K(0));
	hash_remove(t, K(1));
	EXPECT_EQ(g_evict_count, 1);
	hash_remove(t, K(2));
	EXPECT_EQ(g_evict_count, 2);

	hash_destroy(t);
	return 0;
}

KTEST(hash, evict_on_duplicate_insert)
{
	hash_table *t = hash_create(NULL, counting_evict);
	g_evict_count = 0;

	hash_insert(t, K(5), K(10));
	hash_insert(t, K(5), K(20)); /* duplicate — new pair evicted */
	EXPECT_EQ(g_evict_count, 1);

	hash_destroy(t);
	return 0;
}

KTEST(hash, evict_on_destroy)
{
	hash_table *t = hash_create(NULL, counting_evict);
	g_evict_count = 0;

	hash_insert(t, K(1), K(0));
	hash_insert(t, K(2), K(0));
	hash_insert(t, K(3), K(0));
	hash_destroy(t);
	EXPECT_EQ(g_evict_count, 3);
	return 0;
}

/* ── custom comparator ───────────────────────────────────────────────────── */

KTEST(hash, custom_str_comparator)
{
	hash_table *t = hash_create(str_comp, NULL);

	hash_insert(t, "banana", K(2));
	hash_insert(t, "apple", K(1));
	hash_insert(t, "cherry", K(3));

	key_value_pair *p;

	p = hash_find(t, "apple");
	ASSERT_NONNULL(p);
	EXPECT_EQ(V(p->val), 1);

	p = hash_find(t, "banana");
	ASSERT_NONNULL(p);
	EXPECT_EQ(V(p->val), 2);

	EXPECT_NULL(hash_find(t, "date"));

	hash_destroy(t);
	return 0;
}

KTEST(hash, custom_str_comparator_order)
{
	hash_table *t = hash_create(str_comp, NULL);

	hash_insert(t, "dog", K(0));
	hash_insert(t, "ant", K(0));
	hash_insert(t, "cat", K(0));
	hash_insert(t, "bee", K(0));

	/* In-order traversal must yield alphabetical order. */
	const char *expected[] = { "ant", "bee", "cat", "dog" };
	int i = 0;
	key_value_pair *p;
	for (p = hash_first(t); p; p = hash_next(t, p), i++)
		EXPECT_EQ(strcmp((const char *)p->key, expected[i]), 0);
	EXPECT_EQ(i, 4);

	hash_destroy(t);
	return 0;
}
