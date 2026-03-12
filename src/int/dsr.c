#include <int/dsr.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/list.h>

static list_entry dsr_head;
static list_entry dsr_cache;
static spinlock_t dsr_lock;

void dsr_init()
{
	list_init(&dsr_head);
	list_init(&dsr_cache);
	spinlock_init(&dsr_lock);

	for (int i = 0; i < DSR_CACHE_DEPTH; i++) {
		dsr_node *node = kmalloc(sizeof(*node));
		list_init(&node->dsr_list);
		list_insert_tail(&dsr_cache, &node->dsr_list);
	}
}

void dsr_add(dsr_callback fn, void *param)
{
	dsr_node *node = NULL;

	spinlock_lock(&dsr_lock);

	if (!list_is_empty(&dsr_cache)) {
		node = list_remove_head(&dsr_cache);
	}

	if (node) {
		list_init(&node->dsr_list);
		node->fn = fn;
		node->param = param;
		list_insert_tail(&dsr_head, &node->dsr_list);
	}

	spinlock_unlock(&dsr_lock);
}

void dsr_drain()
{
	dsr_node *dsr;
	spinlock_lock(&dsr_lock);
	while (!list_is_empty(&dsr_head)) {
		dsr = container_of(list_remove_head(&dsr_head), dsr_node,
				   dsr_list);
		spinlock_unlock(&dsr_lock);

		if (dsr->fn)
			dsr->fn(dsr->param);

		spinlock_lock(&dsr_lock);

		// give back to dsr cache
		list_init(&dsr->dsr_list);
		list_insert_tail(&dsr_cache, &dsr->dsr_list);
	}
	spinlock_unlock(&dsr_lock);
}
