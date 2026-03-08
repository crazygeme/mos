#include <dsr.h>
#include <klib.h>
#include <lock.h>

static list_entry dsr_head;
static spinlock_t dsr_lock;

void dsr_init()
{
	list_init(&dsr_head);
	spinlock_init(&dsr_lock);
}

void dsr_add(dsr_callback fn, void *param)
{
	dsr_node *node = kmalloc(sizeof(*node));
	if (!node)
		return;
	node->fn = fn;
	node->param = param;
	spinlock_lock(&dsr_lock);
	list_insert_tail(&dsr_head, &node->dsr_list);
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
		kfree(dsr);
		spinlock_lock(&dsr_lock);
	}
	spinlock_unlock(&dsr_lock);
}
