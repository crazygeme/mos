/*
Red Black Trees
(C) 1999  Andrea Arcangeli <andrea@suse.de>

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

linux/include/linux/rbtree.h

To use rbtrees you'll have to implement your own insert and search cores.
This will avoid us to use callbacks and to drop drammatically performances.
I know it's not the cleaner way,  but in C (not in C++) to get
performances and genericity...

Some example of insert and search follows here. The search is a plain
normal search over an ordered tree. The insert instead must be implemented
in two steps: First, the code must insert the element in order as a red leaf
in the tree, and then the support library function rb_insert_color() must
be called. Such function will do the not trivial work to rebalance the
rbtree, if necessary.

-----------------------------------------------------------------------
static INLINE struct page * rb_search_page_cache(struct inode * inode,
unsigned long offset)
{
struct rb_node * n = inode->i_rb_page_cache.rb_node;
struct page * page;

while (n)
{
page = rb_entry(n, struct page, rb_page_cache);

if (offset < page->offset)
n = n->rb_left;
else if (offset > page->offset)
n = n->rb_right;
else
return page;
}
return NULL;
}

static INLINE struct page * __rb_insert_page_cache(struct inode * inode,
unsigned long offset,
struct rb_node * node)
{
struct rb_node ** p = &inode->i_rb_page_cache.rb_node;
struct rb_node * parent = NULL;
struct page * page;

while (*p)
{
parent = *p;
page = rb_entry(parent, struct page, rb_page_cache);

if (offset < page->offset)
p = &(*p)->rb_left;
else if (offset > page->offset)
p = &(*p)->rb_right;
else
return page;
}

rb_link_node(node, parent, p);

return NULL;
}

static INLINE struct page * rb_insert_page_cache(struct inode * inode,
unsigned long offset,
struct rb_node * node)
{
struct page * ret;
if ((ret = __rb_insert_page_cache(inode, offset, node)))
goto out;
rb_insert_color(node, &inode->i_rb_page_cache);
out:
return ret;
}
-----------------------------------------------------------------------
*/

#ifndef	_LINUX_RBTREE_H
#define	_LINUX_RBTREE_H
#ifdef WIN32
#define _TIME_T_DEFINED
#define _CTYPE_DEFINED
#include <windows.h>
#include <osdep.h>
#else
#include <klib.h>
#include <list.h>
#include <lock.h>
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef WIN32
#define INLINE inline
#define container_of CONTAINER_OF
#else
#define INLINE 
#define container_of CONTAINER_OF
#endif

struct rb_node
{
	unsigned long  rb_parent_color;
#define	RB_RED		0
#define	RB_BLACK	1
	struct rb_node *rb_right;
	struct rb_node *rb_left;
};
/* The alignment might seem pointless, but allegedly CRIS needs it */

struct rb_root
{
	struct rb_node *rb_node;
};


#define rb_parent(r)   ((struct rb_node *)((r)->rb_parent_color & ~3))
#define rb_color(r)   ((r)->rb_parent_color & 1)
#define rb_is_red(r)   (!rb_color(r))
#define rb_is_black(r) rb_color(r)
#define rb_set_red(r)  do { (r)->rb_parent_color &= ~1; } while (0)
#define rb_set_black(r)  do { (r)->rb_parent_color |= 1; } while (0)

static INLINE void rb_set_parent(struct rb_node *rb, struct rb_node *p)
{
	rb->rb_parent_color = (rb->rb_parent_color & 3) | (unsigned long)p;
}
static INLINE void rb_set_color(struct rb_node *rb, int color)
{
	rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
}

#define RB_ROOT	(struct rb_root) { NULL, }
#define	rb_entry(ptr, type, member) container_of(ptr, type, member)

#define RB_EMPTY_ROOT(root)	((root)->rb_node == NULL)
#define RB_EMPTY_NODE(node)	(rb_parent(node) == node)
#define RB_CLEAR_NODE(node)	(rb_set_parent(node, node))

static INLINE void rb_init_node(struct rb_node *rb)
{
	rb->rb_parent_color = 0;
	rb->rb_right = NULL;
	rb->rb_left = NULL;
	RB_CLEAR_NODE(rb);
}

extern void rb_insert_color(struct rb_node *, struct rb_root *);
extern void rb_erase(struct rb_node *, struct rb_root *);

typedef void(*rb_augment_f)(struct rb_node *node, void *data);

extern void rb_augment_insert(struct rb_node *node,
	rb_augment_f func, void *data);
extern struct rb_node *rb_augment_erase_begin(struct rb_node *node);
extern void rb_augment_erase_end(struct rb_node *node,
	rb_augment_f func, void *data);

/* Find logical next and previous nodes in a tree */
extern struct rb_node *rb_next(const struct rb_node *);
extern struct rb_node *rb_prev(const struct rb_node *);
extern struct rb_node *rb_first(const struct rb_root *);
extern struct rb_node *rb_last(const struct rb_root *);

/* Fast replacement of a single node without remove/rebalance/add/rebalance */
extern void rb_replace_node(struct rb_node *victim, struct rb_node *new,
struct rb_root *root);

static INLINE void rb_link_node(struct rb_node * node, struct rb_node * parent,
struct rb_node ** rb_link)
{
	node->rb_parent_color = (unsigned long)parent;
	node->rb_left = node->rb_right = NULL;

	*rb_link = node;
}


/**
 * Now for hash table based on rb_tree
 */

typedef struct _hash_table hash_table;
typedef struct _key_value_pair key_value_pair;
typedef int(*hash_comp_fn)(void* my_key, void* in_node);

struct _hash_table
{
	struct rb_root root;
	hash_comp_fn comp;
	spinlock lock;
};

struct _key_value_pair
{
	void* key;
	void* val;
	struct rb_node node;
};

hash_table* hash_create(hash_comp_fn comp);

int hash_destroy(hash_table* table);

int hash_insert(hash_table* table, void* key, void* val);

int hash_remove(hash_table* table, void* key);

key_value_pair* hash_find(hash_table* table, void* key);

int hash_update(hash_table* table, void* key, void* val);

key_value_pair* hash_first(hash_table* table);

key_value_pair* hash_next(hash_table* table, key_value_pair* pair);

int hash_isempty(hash_table* table);

#ifdef DEBUG_RB

void hash_print(hash_table* table);

#endif

#endif	/* _LINUX_RBTREE_H */

