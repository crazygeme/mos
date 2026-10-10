#include <int/dsr.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/list.h>
#include <ps/ps.h>
#include <macro.h>

static list_entry dsr_head;
static list_entry dsr_cache;
static spinlock_t dsr_lock;
static task_struct *dsr_task;
static int dsr_waiting;
static void dsr_drain(void);

static int dsr_dropped; /* nodes lost due to cache exhaustion; grown in drain */

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

int dsr_add(dsr_callback fn, void *param)
{
	list_entry *node = NULL;
	int irq;

	spinlock_lock(&dsr_lock, &irq);

	if (!list_is_empty(&dsr_cache))
		node = list_remove_head(&dsr_cache);

	if (node) {
		dsr_node *dsr = container_of(node, dsr_node, dsr_list);
		list_init(&dsr->dsr_list);
		dsr->fn = fn;
		dsr->param = param;
		list_insert_tail(&dsr_head, &dsr->dsr_list);
	} else {
		dsr_dropped++;
	}

	/* Only wake the empty-queue wait, never a wait inside a callback. */
	if (node && dsr_waiting) {
		dsr_waiting = 0;
		ps_put_to_ready_queue(dsr_task);
	}

	spinlock_unlock(&dsr_lock, irq);
	return node != NULL;
}

static void dsr_drain(void)
{
	dsr_node *dsr;
	int irq;

	spinlock_lock(&dsr_lock, &irq);
	while (!list_is_empty(&dsr_head)) {
		dsr = container_of(list_remove_head(&dsr_head), dsr_node,
				   dsr_list);
		spinlock_unlock(&dsr_lock, irq);

		if (dsr->fn)
			dsr->fn(dsr->param);

		spinlock_lock(&dsr_lock, &irq);

		// give back to dsr cache
		list_init(&dsr->dsr_list);
		list_insert_tail(&dsr_cache, &dsr->dsr_list);
	}

	/* Grow the pool to replace nodes lost during the burst.  We are in
	 * task context here so kmalloc is safe. */
	int grow = dsr_dropped;
	dsr_dropped = 0;
	spinlock_unlock(&dsr_lock, irq);

	if (grow)
		klog("dsr: cache exhausted, growing pool by %d nodes\n", grow);

	for (int i = 0; i < grow; i++) {
		dsr_node *node = kmalloc(sizeof(*node));
		list_init(&node->dsr_list);
		spinlock_lock(&dsr_lock, &irq);
		list_insert_tail(&dsr_cache, &node->dsr_list);
		spinlock_unlock(&dsr_lock, irq);
	}
}

/* Callbacks own a task stack and may yield or wait without suspending the
 * scheduler or borrowing the state of an interrupted task. */
static void dsr_worker(void *param)
{
	int irq;
	(void)param;

	for (;;) {
		dsr_drain();
		spinlock_lock(&dsr_lock, &irq);
		if (!list_is_empty(&dsr_head)) {
			spinlock_unlock(&dsr_lock, irq);
			continue;
		}
		dsr_waiting = 1;
		ps_put_to_wait_queue(current, NULL);
		spinlock_unlock(&dsr_lock, irq);
		task_sched();
	}
}

/* Called after PID 0 and PID 1 are allocated, before scheduler kickoff. */
void dsr_start(void)
{
	unsigned pid = ps_create(dsr_worker, NULL, ps_deferred, ps_kernel);
	dsr_task = ps_find_process(pid);
	if (!dsr_task)
		DIE();
}

/* Scheduler state is sampled under ps_lock. */
int dsr_needs_schedule(void)
{
	return dsr_task && dsr_task != current && ps_task_ready(dsr_task);
}
