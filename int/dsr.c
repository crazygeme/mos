#include <int/dsr.h>
#include <lib/klib.h>
#include <int/int.h>
#include <ps/lock.h>

#define SOFTIRQ 0x1
#define TRIGGER_SOFTIRQ() \
	__asm__ ("int $1");

static LIST_ENTRY dsr_head;
static spinlock dsr_lock;

static void lock_dsr()
{
	spinlock_lock(&dsr_lock);
}

static void unlock_dsr()
{
	spinlock_unlock(&dsr_lock);
}

static void dsr_interrupt(intr_frame* frame)
{
	dsr_process();
}

void dsr_init()
{
	InitializeListHead(&dsr_head);
	spinlock_init(&dsr_lock);
	//int_register(SOFTIRQ, dsr_interrupt, 1, 0);
}

void dsr_add(dsr_callback fn, void* param)
{
	dsr_node* node = kmalloc(sizeof(*node));
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
		kfree(dsr);
		lock_dsr();
	}
	unlock_dsr();
}


