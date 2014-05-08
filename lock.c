#include "lock.h"
#include "ps.h"
#include "klib.h"

static void waitable_get(void* waitable);

static void waitable_put(void* waitable);

void spinlock_init(spinlock_t lock)
{
	lock->lock = 0;
	lock->inited = 1;
	lock->int_status = 0;
}

void spinlock_uninit(spinlock_t lock)
{
	lock->inited = 0;
	//spinlock_put(&lock->lock);
		__sync_lock_test_and_set( &(lock->lock), 0);
		if (lock->int_status == 1){
			int_intr_enable();
		}else{
			int_intr_disable();
		}
}

// FIXME
// spin lock implement has some problem

void spinlock_lock(spinlock_t lock)
{
	if (lock->inited){
		lock->int_status = int_intr_disable();
	  //spinlock_get(&lock->lock);
		while( __sync_lock_test_and_set( &(lock->lock), 1) == 1)
		  ;
	}
}

void spinlock_unlock(spinlock_t lock)
{
	if (lock->inited){
	  //spinlock_put(&lock->lock);
		__sync_lock_test_and_set( &(lock->lock), 0);
		if (lock->int_status == 1){
			int_intr_enable();
		}else{
			int_intr_disable();
		}
	}
}



event* create_event(wait_type type, unsigned initial_state, const char* _name)
{
	event* e = (event*)kmalloc(sizeof(event));
	waitable_object *obj = (waitable_object*)e;

	memset(e, 0, sizeof(*e));
	obj->type = type;
	obj->lock = initial_state;
	obj->depth = 0;
	InitializeListHead( &(obj->wait_list) );
	
	if (_name)
	  strcpy(obj->name, _name);
	else
	  *(e->obj.name) = '0';
	//printk("current stack %x, e %x, rodata %x\n", &e, e, _name);
	//printk("e %x, obj %x, sizeof event is %x, name %x, param %x\n",e,obj, sizeof(*e), obj->name, _name);
	return e;
}

void delete_event(event* e)
{
	kfree(e);
}

static void waitable_get(void* waitable)
{
	waitable_object* obj = (waitable_object*)waitable; 
	if (obj->type == wait_type_lock){
	
		while(__sync_lock_test_and_set(&(obj->lock), 1) == 1){
			obj->depth++;
			task_sched_wait( &( obj->wait_list) );
			obj->depth--;
		}

	}else{
	
		if (__sync_lock_test_and_set(&(obj->lock), 1) == 1){
			// fail to get lock , go into wait list
			obj->depth++;
			task_sched_wait( &( obj->wait_list) );
			obj->depth--;
		}
	}
}

static void waitable_put(void* waitable)
{
	waitable_object* obj = (waitable_object*)waitable;
	task_sched_wakeup(&( obj->wait_list), (obj->type == wait_type_notify));
	__sync_lock_test_and_set(&(obj->lock), 0);
	
	if (obj->type == wait_type_lock)
	  task_sched();
	else
	  __sync_lock_test_and_set(&(obj->lock), 1); 
}

void wait_event(event* e)
{
    waitable_get(e);
}

void set_event(event* e)
{
    waitable_put(e);
}

void sema_init(semaphore* s, const char* name, unsigned int initstat)
{
	if (name && *name)
	  strcpy(s->name, name);
	else
	  *s->name = '\0';

	s->lock = initstat;
}

void sema_wait(semaphore* s)
{
	while(__sync_lock_test_and_set(&(s->lock), 1) == 1){ 
		task_sched();
	}
}

void sema_reset(semaphore* s)
{
    __sync_lock_test_and_set(&(s->lock), 1);
}

void sema_trigger(semaphore* s)
{
	__sync_lock_test_and_set(&(s->lock), 0);
    task_sched();
}




#ifdef TEST_LOCK
#ifndef TEST_PS

static semaphore sema;
static void main_process(void* param)
{
	/*
	wait_type type = (wait_type)param; 
	int i;
	printk("main_process\n");
	waitable_get(test_event);

	msleep(1000);

	if (type == wait_type_event){
		for( i = 0; i < TEST_LOCK; i++)
			waitable_put(test_event);
	}else{
		waitable_put(test_event);
	}
	*/

	int i;

	for (i = 0; i < TEST_LOCK; i++){
		msleep(100);
		printk("main process trigger semaphore\n");
		sema_trigger(&sema);
	}
	
	while(1)
    {
        ps_cleanup_dying_task();
    }
}

static void wait_process(void* param)
{
	/*
	wait_type type = (wait_type)param;
	task_struct* cur = CURRENT_TASK();
	if (type != wait_type_lock){
		printk("%d wait_process\n", cur->psid); 
		waitable_get(test_event);
		printk("%d wakeup, depth %d\n", cur->psid, test_event->obj.depth);
	
	}else{
		waitable_get(test_event);
		printk("%d get lock\n", cur->psid);
		msleep(200);
		printk("%d put lock\n", cur->psid);
		waitable_put(test_event);
	}
	*/
	task_struct* cur = CURRENT_TASK(); 
	sema_wait(&sema);
	printk("process %d triggered\n", cur->psid);
}

void test_event_process()
{
	/*
	int i = 0;
	wait_type type = wait_type_notify;
	printk("test_event_process\n");
	test_event = create_event(type, 0, "event");
	printk("create event is %x, wait_list %x\n", test_event, &(test_event->obj.wait_list));
	printk("wait_list flink %x blikn %x\n", test_event->obj.wait_list.Flink,
				test_event->obj.wait_list.Blink);
	ps_create(main_process, (void*)type, 1, ps_kernel);
	
	for(i = 0; i < TEST_LOCK; i++){
		proc[i] = ps_create(wait_process, (void*)type, 1, ps_kernel);
	}
	*/
	int i = 0;
	sema_init(&sema, "test", 1);

	ps_create(main_process, 0, 1, ps_kernel); 
	for(i = 0; i < TEST_LOCK; i++){
		ps_create(wait_process, 0, 1, ps_kernel);
	}

	ps_kickoff();
}

#endif
#endif
