#include <int/dsr.h>
#include <lib/klib.h>
#include <int/int.h>
#include <ps/lock.h>
#include <config.h>

#define SOFTIRQ 0x1
#define TRIGGER_SOFTIRQ() \
	__asm__ ("int $1");



static LIST_ENTRY dsr_head;
static spinlock dsr_lock;
static spinlock cache_lock;
static LIST_ENTRY dsr_cache;

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

static void dsr_interrupt(intr_frame* frame)
{
	dsr_process();
}

static dsr_node* dsr_alloc_node()
{
	dsr_node* dsr = 0;
	LIST_ENTRY* node = 0;
	lock_cache();
	if(!IsListEmpty(&dsr_cache)) {
		node = RemoveHeadList(&dsr_cache);
		dsr = CONTAINER_OF(node, dsr_node, dsr_list);
	}else{
		dsr = 0;
	}
	unlock_cache();

	return dsr;
}

static void dsr_free_node(dsr_node* node)
{
	lock_cache();

	InsertHeadList(&dsr_cache, &(node->dsr_list));

	unlock_cache();
}

void dsr_init()
{
	int i = 0;
	InitializeListHead(&dsr_head);
	InitializeListHead(&dsr_cache);
	spinlock_init(&dsr_lock);
	spinlock_init(&cache_lock);

	for(i = 0; i < DSR_CACHE_DEPTH; i++) {
		dsr_node* node = kmalloc(sizeof(*node));
		InsertHeadList(&dsr_cache, &(node->dsr_list));
	}
	//int_register(SOFTIRQ, dsr_interrupt, 1, 0);
}

void dsr_add(dsr_callback fn, void* param)
{
	dsr_node* node = 0;

	node = dsr_alloc_node();
	if(!node) {
		return;
	}
	memset(node, 0, sizeof(*node));
	node->fn = fn;
	node->param = param;
	lock_dsr();
	InsertTailList(&dsr_head, &(node->dsr_list));	
	unlock_dsr();
	//TRIGGER_SOFTIRQ();

}

void dsr_process()
{
	LIST_ENTRY* node;
	dsr_node *dsr;
	lock_dsr();
	while( !IsListEmpty(&dsr_head) ){
		node = RemoveHeadList(&dsr_head);
		dsr = CONTAINER_OF(node, dsr_node, dsr_list);
		unlock_dsr();
		if (dsr->fn){
			dsr->fn(dsr->param);
		}
		dsr_free_node(dsr);
		lock_dsr();
	}
	unlock_dsr();
}


