#include <dsr.h>
#include <klib.h>
#include <int.h>
#include <lock.h>
#include <config.h>
#include <ps.h>

#define SOFTIRQ 0x1
#define TRIGGER_SOFTIRQ() asm volatile("int $1");

static list_entry dsr_head;
static spinlock dsr_lock;
static spinlock cache_lock;
static list_entry dsr_cache;

static void lock_dsr()
{
	spinlock_lock(&dsr_lock);
}

static void unlock_dsr()
{
	spinlock_unlock(&dsr_lock);
}

static void lock_cache()
{
	spinlock_lock(&cache_lock);
}

static void unlock_cache()
{
	spinlock_unlock(&cache_lock);
}

static void dsr_interrupt(intr_frame *frame)
{
	dsr_process();
}

static dsr_node *dsr_alloc_node()
{
	dsr_node *dsr = 0;
	list_entry *node = 0;
	lock_cache();
	if (!list_is_empty(&dsr_cache)) {
		node = list_remove_head(&dsr_cache);
		dsr = container_of(node, dsr_node, dsr_list);
	} else {
		dsr = 0;
	}
	unlock_cache();

	return dsr;
}

static void dsr_free_node(dsr_node *node)
{
	lock_cache();

	list_insert_head(&dsr_cache, &(node->dsr_list));

	unlock_cache();
}

void dsr_init()
{
	int i = 0;
	list_init(&dsr_head);
	list_init(&dsr_cache);
	spinlock_init(&dsr_lock);
	spinlock_init(&cache_lock);

	for (i = 0; i < DSR_CACHE_DEPTH; i++) {
		dsr_node *node = kmalloc(sizeof(*node));
		list_insert_head(&dsr_cache, &(node->dsr_list));
	}
	// int_register(SOFTIRQ, dsr_interrupt, 1, 0);
}

void dsr_add(dsr_callback fn, void *param)
{
	dsr_node *node = 0;
	node = dsr_alloc_node();
	if (!node) {
		return;
	}
	memset(node, 0, sizeof(*node));
	node->fn = fn;
	node->param = param;
	lock_dsr();
	list_insert_tail(&dsr_head, &(node->dsr_list));
	unlock_dsr();
	// TRIGGER_SOFTIRQ();
}

int dsr_has_task()
{
	int has = 0;
	lock_dsr();
	has = !list_is_empty(&dsr_head);
	unlock_dsr();
	return has;
}

static int is_dsr_running = 0;

int dsr_running()
{
	int ret = 0;
	lock_dsr();
	ret = is_dsr_running;
	unlock_dsr();
	return ret;
}

void dsr_process()
{
	list_entry *node;
	dsr_node *dsr;
	do {
		lock_dsr();
		is_dsr_running = 1;
		while (!list_is_empty(&dsr_head)) {
			node = list_remove_head(&dsr_head);
			unlock_dsr();
			dsr = container_of(node, dsr_node, dsr_list);
			if (dsr->fn) {
				dsr->fn(dsr->param);
			}
			dsr_free_node(dsr);
			lock_dsr();
		}
		is_dsr_running = 0;
		unlock_dsr();
		task_sched();
	} while (1);
}
