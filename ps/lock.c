#include <ps/lock.h>
#include <ps/ps.h>
#include <lib/klib.h>

static void waitable_get(void* waitable);

static void waitable_put(void* waitable);

void spinlock_init(spinlock_t lock)
{
	lock->lock = 0;
	lock->inited = 1;
	lock->int_status = 0;
	lock->holder = 0;
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


void spinlock_lock(spinlock_t lock)
{
	task_struct* cur = CURRENT_TASK();
	if (lock->inited){
		lock->int_status = int_intr_disable();
	  //spinlock_get(&lock->lock);
		while( __sync_lock_test_and_set( &(lock->lock), 1) == 1)
		  ;

		lock->holder = cur->psid;
	}
}

void spinlock_unlock(spinlock_t lock)
{
	task_struct* cur = CURRENT_TASK();
	if (lock->inited){
	  //spinlock_put(&lock->lock);
		lock->holder = 0;
		__sync_lock_test_and_set( &(lock->lock), 0);
		if (lock->int_status == 1){
			int_intr_enable();
		}else{
			int_intr_disable();
		}
	}
}



void sema_init(semaphore* s, const char* name, unsigned int initstat)
{
	if (name && *name)
	  strcpy(s->name, name);
	else
	  *s->name = '\0';

	s->lock = initstat;
    InitializeListHead(&s->wait_list);
    spinlock_init(&s->wait_lock);
}

void sema_wait(semaphore* s)
{
    task_struct* cur = CURRENT_TASK();
	while(__sync_lock_test_and_set(&(s->lock), 1) == 1){ 
        // change myself into waiting status
        spinlock_lock(&s->wait_lock);
        cur->status = ps_waiting;
        ps_put_to_wait_queue(cur);
        // then add to sema wait list
        InsertTailList(&s->wait_list, &cur->lock_list);
        spinlock_unlock(&s->wait_lock);
		task_sched();
	}
}

void sema_reset(semaphore* s)
{
    __sync_lock_test_and_set(&(s->lock), 1);
}


static void sema_notice_one(semaphore* s)
{
    task_struct* task = 0;
    LIST_ENTRY* entry = 0;
    spinlock_lock(&s->wait_lock);
    // get a waiting task is has
    if (!IsListEmpty(&s->wait_list)) {
        entry = RemoveHeadList(&s->wait_list);
        task = CONTAINER_OF(entry, task_struct, lock_list);
        // then put it into ready status
        task->status = ps_ready;
        ps_put_to_ready_queue(task);
    }
    spinlock_unlock(&s->wait_lock);
}

void sema_trigger(semaphore* s)
{
    sema_notice_one(s);

	__sync_lock_test_and_set(&(s->lock), 0);
    task_sched();
}

void sema_wait_for_intr(semaphore* s)
{
	while(__sync_lock_test_and_set(&(s->lock), 1) == 1){ 
		task_sched();
	}
}

void sema_trigger_at_intr(semaphore* s)
{
	__sync_lock_test_and_set(&(s->lock), 0);
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
