/* Unit tests for the intrusive red-black tree primitives. */

#include <lib/rbtree.h>
#include <test/test.h>

typedef struct _test_rb_entry {
	int key;
	struct rb_node node;
} test_rb_entry;

static int test_rb_insert(struct rb_root *root, test_rb_entry *entry)
{
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		test_rb_entry *current = rb_entry(*link, test_rb_entry, node);
		parent = *link;
		if (entry->key < current->key)
			link = &(*link)->rb_left;
		else if (entry->key > current->key)
			link = &(*link)->rb_right;
		else
			return 0;
	}
	rb_link_node(&entry->node, parent, link);
	rb_insert_color(&entry->node, root);
	return 1;
}

KTEST(rbtree, insert_iterate_erase)
{
	struct rb_root root = _RBTREE_ROOT_INIT;
	test_rb_entry entries[5];
	int keys[] = { 4, 1, 5, 2, 3 };
	struct rb_node *node;
	int i;

	for (i = 0; i < 5; i++) {
		entries[i].key = keys[i];
		rb_init_node(&entries[i].node);
		EXPECT_TRUE(test_rb_insert(&root, &entries[i]));
	}

	i = 1;
	for (node = rb_first(&root); node; node = rb_next(node), i++) {
		test_rb_entry *entry = rb_entry(node, test_rb_entry, node);
		EXPECT_EQ(entry->key, i);
	}
	EXPECT_EQ(i, 6);

	rb_erase(&entries[0].node, &root);
	{
		test_rb_entry *first =
			rb_entry(rb_first(&root), test_rb_entry, node);
		test_rb_entry *last =
			rb_entry(rb_last(&root), test_rb_entry, node);
		EXPECT_EQ(first->key, 1);
		EXPECT_EQ(last->key, 5);
	}
	return 0;
}

KTEST(rbtree, duplicate_rejected)
{
	struct rb_root root = _RBTREE_ROOT_INIT;
	test_rb_entry first = { 7, { 0, NULL, NULL } };
	test_rb_entry duplicate = { 7, { 0, NULL, NULL } };

	rb_init_node(&first.node);
	rb_init_node(&duplicate.node);
	EXPECT_TRUE(test_rb_insert(&root, &first));
	EXPECT_FALSE(test_rb_insert(&root, &duplicate));
	return 0;
}
