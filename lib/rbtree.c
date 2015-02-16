/*
Red Black Trees
(C) 1999  Andrea Arcangeli <andrea@suse.de>
(C) 2002  David Woodhouse <dwmw2@infradead.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

linux/lib/rbtree.c
*/

#include <lib/rbtree.h>

static void __rb_rotate_left(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *right = node->rb_right;
	struct rb_node *parent = rb_parent(node);

	if ((node->rb_right = right->rb_left))
		rb_set_parent(right->rb_left, node);
	right->rb_left = node;

	rb_set_parent(right, parent);

	if (parent)
	{
		if (node == parent->rb_left)
			parent->rb_left = right;
		else
			parent->rb_right = right;
	}
	else
		root->rb_node = right;
	rb_set_parent(node, right);
}

static void __rb_rotate_right(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *left = node->rb_left;
	struct rb_node *parent = rb_parent(node);

	if ((node->rb_left = left->rb_right))
		rb_set_parent(left->rb_right, node);
	left->rb_right = node;

	rb_set_parent(left, parent);

	if (parent)
	{
		if (node == parent->rb_right)
			parent->rb_right = left;
		else
			parent->rb_left = left;
	}
	else
		root->rb_node = left;
	rb_set_parent(node, left);
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *parent, *gparent;

	while ((parent = rb_parent(node)) && rb_is_red(parent))
	{
		gparent = rb_parent(parent);

		if (parent == gparent->rb_left)
		{
			{
				register struct rb_node *uncle = gparent->rb_right;
				if (uncle && rb_is_red(uncle))
				{
					rb_set_black(uncle);
					rb_set_black(parent);
					rb_set_red(gparent);
					node = gparent;
					continue;
				}
			}

			if (parent->rb_right == node)
			{
				register struct rb_node *tmp;
				__rb_rotate_left(parent, root);
				tmp = parent;
				parent = node;
				node = tmp;
			}

			rb_set_black(parent);
			rb_set_red(gparent);
			__rb_rotate_right(gparent, root);
		}
		else {
			{
				register struct rb_node *uncle = gparent->rb_left;
				if (uncle && rb_is_red(uncle))
				{
					rb_set_black(uncle);
					rb_set_black(parent);
					rb_set_red(gparent);
					node = gparent;
					continue;
				}
			}

			if (parent->rb_left == node)
			{
				register struct rb_node *tmp;
				__rb_rotate_right(parent, root);
				tmp = parent;
				parent = node;
				node = tmp;
			}

			rb_set_black(parent);
			rb_set_red(gparent);
			__rb_rotate_left(gparent, root);
		}
	}

	rb_set_black(root->rb_node);
}
// EXPORT_SYMBOL(rb_insert_color);

static void __rb_erase_color(struct rb_node *node, struct rb_node *parent,
struct rb_root *root)
{
	struct rb_node *other;

	while ((!node || rb_is_black(node)) && node != root->rb_node)
	{
		if (parent->rb_left == node)
		{
			other = parent->rb_right;
			if (rb_is_red(other))
			{
				rb_set_black(other);
				rb_set_red(parent);
				__rb_rotate_left(parent, root);
				other = parent->rb_right;
			}
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
				(!other->rb_right || rb_is_black(other->rb_right)))
			{
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			}
			else
			{
				if (!other->rb_right || rb_is_black(other->rb_right))
				{
					rb_set_black(other->rb_left);
					rb_set_red(other);
					__rb_rotate_right(other, root);
					other = parent->rb_right;
				}
				rb_set_color(other, rb_color(parent));
				rb_set_black(parent);
				rb_set_black(other->rb_right);
				__rb_rotate_left(parent, root);
				node = root->rb_node;
				break;
			}
		}
		else
		{
			other = parent->rb_left;
			if (rb_is_red(other))
			{
				rb_set_black(other);
				rb_set_red(parent);
				__rb_rotate_right(parent, root);
				other = parent->rb_left;
			}
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
				(!other->rb_right || rb_is_black(other->rb_right)))
			{
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			}
			else
			{
				if (!other->rb_left || rb_is_black(other->rb_left))
				{
					rb_set_black(other->rb_right);
					rb_set_red(other);
					__rb_rotate_left(other, root);
					other = parent->rb_left;
				}
				rb_set_color(other, rb_color(parent));
				rb_set_black(parent);
				rb_set_black(other->rb_left);
				__rb_rotate_right(parent, root);
				node = root->rb_node;
				break;
			}
		}
	}
	if (node)
		rb_set_black(node);
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *child, *parent;
	int color;

	if (!node->rb_left)
		child = node->rb_right;
	else if (!node->rb_right)
		child = node->rb_left;
	else
	{
		struct rb_node *old = node, *left;

		node = node->rb_right;
		while ((left = node->rb_left) != NULL)
			node = left;

		if (rb_parent(old)) {
			if (rb_parent(old)->rb_left == old)
				rb_parent(old)->rb_left = node;
			else
				rb_parent(old)->rb_right = node;
		}
		else
			root->rb_node = node;

		child = node->rb_right;
		parent = rb_parent(node);
		color = rb_color(node);

		if (parent == old) {
			parent = node;
		}
		else {
			if (child)
				rb_set_parent(child, parent);
			parent->rb_left = child;

			node->rb_right = old->rb_right;
			rb_set_parent(old->rb_right, node);
		}

		node->rb_parent_color = old->rb_parent_color;
		node->rb_left = old->rb_left;
		rb_set_parent(old->rb_left, node);

		goto color;
	}

	parent = rb_parent(node);
	color = rb_color(node);

	if (child)
		rb_set_parent(child, parent);
	if (parent)
	{
		if (parent->rb_left == node)
			parent->rb_left = child;
		else
			parent->rb_right = child;
	}
	else
		root->rb_node = child;

color:
	if (color == RB_BLACK)
		__rb_erase_color(child, parent, root);
}
// EXPORT_SYMBOL(rb_erase);

static void rb_augment_path(struct rb_node *node, rb_augment_f func, void *data)
{
	struct rb_node *parent;

up:
	func(node, data);
	parent = rb_parent(node);
	if (!parent)
		return;

	if (node == parent->rb_left && parent->rb_right)
		func(parent->rb_right, data);
	else if (parent->rb_left)
		func(parent->rb_left, data);

	node = parent;
	goto up;
}

/*
* after inserting @node into the tree, update the tree to account for
* both the new entry and any damage done by rebalance
*/
void rb_augment_insert(struct rb_node *node, rb_augment_f func, void *data)
{
	if (node->rb_left)
		node = node->rb_left;
	else if (node->rb_right)
		node = node->rb_right;

	rb_augment_path(node, func, data);
}
// EXPORT_SYMBOL(rb_augment_insert);

/*
* before removing the node, find the deepest node on the rebalance path
* that will still be there after @node gets removed
*/
struct rb_node *rb_augment_erase_begin(struct rb_node *node)
{
	struct rb_node *deepest;

	if (!node->rb_right && !node->rb_left)
		deepest = rb_parent(node);
	else if (!node->rb_right)
		deepest = node->rb_left;
	else if (!node->rb_left)
		deepest = node->rb_right;
	else {
		deepest = rb_next(node);
		if (deepest->rb_right)
			deepest = deepest->rb_right;
		else if (rb_parent(deepest) != node)
			deepest = rb_parent(deepest);
	}

	return deepest;
}
// EXPORT_SYMBOL(rb_augment_erase_begin);

/*
* after removal, update the tree to account for the removed entry
* and any rebalance damage.
*/
void rb_augment_erase_end(struct rb_node *node, rb_augment_f func, void *data)
{
	if (node)
		rb_augment_path(node, func, data);
}
// EXPORT_SYMBOL(rb_augment_erase_end);

/*
* This function returns the first node (in sort order) of the tree.
*/
struct rb_node *rb_first(const struct rb_root *root)
{
	struct rb_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}
// EXPORT_SYMBOL(rb_first);

struct rb_node *rb_last(const struct rb_root *root)
{
	struct rb_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_right)
		n = n->rb_right;
	return n;
}
// EXPORT_SYMBOL(rb_last);

struct rb_node *rb_next(const struct rb_node *node)
{
	struct rb_node *parent;

	if (rb_parent(node) == node)
		return NULL;

	/* If we have a right-hand child, go down and then left as far
	as we can. */
	if (node->rb_right) {
		node = node->rb_right;
		while (node->rb_left)
			node = node->rb_left;
		return (struct rb_node *)node;
	}

	/* No right-hand children.  Everything down and left is
	smaller than us, so any 'next' node must be in the general
	direction of our parent. Go up the tree; any time the
	ancestor is a right-hand child of its parent, keep going
	up. First time it's a left-hand child of its parent, said
	parent is our 'next' node. */
	while ((parent = rb_parent(node)) && node == parent->rb_right)
		node = parent;

	return parent;
}
// EXPORT_SYMBOL(rb_next);

struct rb_node *rb_prev(const struct rb_node *node)
{
	struct rb_node *parent;

	if (rb_parent(node) == node)
		return NULL;

	/* If we have a left-hand child, go down and then right as far
	as we can. */
	if (node->rb_left) {
		node = node->rb_left;
		while (node->rb_right)
			node = node->rb_right;
		return (struct rb_node *)node;
	}

	/* No left-hand children. Go up till we find an ancestor which
	is a right-hand child of its parent */
	while ((parent = rb_parent(node)) && node == parent->rb_left)
		node = parent;

	return parent;
}
// EXPORT_SYMBOL(rb_prev);

void rb_replace_node(struct rb_node *victim, struct rb_node *new,
struct rb_root *root)
{
	struct rb_node *parent = rb_parent(victim);

	/* Set the surrounding nodes to point to the replacement */
	if (parent) {
		if (victim == parent->rb_left)
			parent->rb_left = new;
		else
			parent->rb_right = new;
	}
	else {
		root->rb_node = new;
	}
	if (victim->rb_left)
		rb_set_parent(victim->rb_left, new);
	if (victim->rb_right)
		rb_set_parent(victim->rb_right, new);

	/* Copy the pointers/colour from the victim to the replacement */
	*new = *victim;
}
// EXPORT_SYMBOL(rb_replace_node);


static INLINE int hash_binary_comp(void* key1, void* key2)
{
	char* k1 = (char*)key1;
	char* k2 = (char*)key2;
	return (k1 - k2);
}


static INLINE int hash_free_node(struct rb_node* node)
{
	key_value_pair* pair = 0;

	if (!node)
		return 0;

	if (node->rb_left)
		hash_free_node(node->rb_left);

	if (node->rb_right)
		hash_free_node(node->rb_right);

	pair = rb_entry(node, key_value_pair, node);
	kfree(pair);

	return 1;
}

hash_table* hash_create(hash_comp_fn comp)
{
	hash_table* table = kmalloc(sizeof(*table));
	table->root.rb_node = 0;
	if (comp)
		table->comp = comp;
	else
		table->comp = hash_binary_comp;

	spinlock_init(&table->lock);

	return table;
}

int hash_destroy(hash_table* table)
{
	int ret = 0;

	if (!table )
		return 0;
	
	spinlock_lock(&table->lock);
    if (table->root.rb_node) {
        ret = hash_free_node(table->root.rb_node); 
    }
	spinlock_unlock(&table->lock);
	kfree(table);
	return 1;
}


static INLINE key_value_pair * __rb_insert(hash_table* table,
	void* key, struct rb_node * node)
{
	struct rb_node ** p = &table->root.rb_node;
	struct rb_node * parent = NULL;
	struct _key_value_pair * pair;
	int comp = 0;

	while (*p)
	{
		parent = *p;
		pair = rb_entry(parent, key_value_pair, node);

		comp = table->comp(key, pair->key);
		if (comp < 0)
			p = &(*p)->rb_left;
		else if (comp > 0)
			p = &(*p)->rb_right;
		else
			return pair;
	}

	rb_link_node(node, parent, p);

	return NULL;
}

static INLINE key_value_pair * __hash_insert(hash_table* table,
	void* key, struct rb_node * node)
{
	key_value_pair * ret = 0;
	if ((ret = __rb_insert(table, key, node)))
		goto out;

	rb_insert_color(node, &table->root);
out:
	return ret;
}

int hash_insert(hash_table* table, void* key, void* val)
{
	key_value_pair* pair = kmalloc(sizeof(*pair));
	key_value_pair* ret = 0;

	pair->key = key;
	pair->val = val;
	rb_init_node(&pair->node);

	spinlock_lock(&table->lock);
	ret = __hash_insert(table, key, &pair->node);
	spinlock_unlock(&table->lock);
	if (ret){
		kfree(pair);
		return 0;
	}

	return 1;
}

int hash_remove(hash_table* table, void* key)
{
	key_value_pair* pair = hash_find(table, key);

	if (!pair)
		return 0;
	spinlock_lock(&table->lock);
	rb_erase(&pair->node, &table->root);
	spinlock_unlock(&table->lock);
	kfree(pair);

	return 1;
}

key_value_pair* hash_find(hash_table* table, void* key)
{
	struct rb_node ** p = &table->root.rb_node;
	struct rb_node *parent;
	struct _key_value_pair * pair;
	int comp = 0;

	spinlock_lock(&table->lock);
	while (*p)
	{
		parent = *p;
		pair = rb_entry(parent, key_value_pair, node);
		comp = table->comp(key, pair->key);
		if (comp < 0)
			p = &(*p)->rb_left;
		else if (comp > 0)
			p = &(*p)->rb_right;
		else{
			spinlock_unlock(&table->lock);
			return pair;
		}
	}
	spinlock_unlock(&table->lock);

	return 0;
}

int hash_update(hash_table* table, void* key, void* val)
{
	key_value_pair* pair = hash_find(table, key);

	if (!pair)
		return 0;

	pair->val = val;

	return 1;
}

key_value_pair* hash_first(hash_table* table)
{
    struct rb_node* first = rb_first( &table->root );
    key_value_pair* pair;

    if (!first) {
        return 0;
    }

    pair = rb_entry(first, key_value_pair, node);
    return pair;
}

key_value_pair* hash_next(hash_table* table, key_value_pair* pair)
{
    struct rb_node* next = rb_next(&pair->node);
    key_value_pair* ret;

    if (!next) {
        return 0;
    }

    ret = rb_entry(next, key_value_pair, node);
    return ret;

}

int hash_isempty(hash_table* table)
{
    if (!table) {
        return 1;
    }

    return (table->root.rb_node == NULL);
}

#ifdef DEBUG_RB

static void hash_print_depth(char* flag, struct rb_node* node, int depth)
{
	int i = 0;
	key_value_pair* pair = rb_entry(node, key_value_pair, node);

	if (!node)
		return;

	for (i = 0; i < depth; i++){
		printf("\t");
	}

	printf("[%c:%.4d:%.4d:%.2d]\n",flag, pair->key, pair->val, rb_color(node));

	hash_print_depth('l', node->rb_left, (depth + 1));
	hash_print_depth('r', node->rb_right, (depth + 1));
}

void hash_print(hash_table* table)
{
	hash_print_depth('o', table->root.rb_node, 0);
}

#endif

