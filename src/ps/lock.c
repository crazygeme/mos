#include <lock.h>
#include <ps.h>
#include <klib.h>

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
    __sync_lock_test_and_set(&(lock->lock), 0);
    if (lock->int_status == 1)
    {
        int_intr_enable();
    }
    else
    {
        int_intr_disable();
    }
}


void spinlock_lock(spinlock_t lock)
{
    task_struct* cur = CURRENT_TASK();
    if (lock->inited)
    {
        lock->int_status = int_intr_disable();
        //spinlock_get(&lock->lock);
        while (__sync_lock_test_and_set(&(lock->lock), 1) == 1)
            ;

        lock->holder = cur->psid;
    }
}

void spinlock_unlock(spinlock_t lock)
{
    task_struct* cur = CURRENT_TASK();
    if (lock->inited)
    {
        //spinlock_put(&lock->lock);
        lock->holder = 0;
        __sync_lock_test_and_set(&(lock->lock), 0);
        if (lock->int_status == 1)
        {
            int_intr_enable();
        }
        else
        {
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
    while (__sync_lock_test_and_set(&(s->lock), 1) == 1)
    {
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
    if (!IsListEmpty(&s->wait_list))
    {
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
    while (__sync_lock_test_and_set(&(s->lock), 1) == 1)
    {
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
    
    int i;

    for (i = 0; i < TEST_LOCK; i++)
    {
        msleep(100);
        printk("main process trigger semaphore\n");
        sema_trigger(&sema);
    }

    while (1)
    {
        ps_cleanup_dying_task();
    }
}

static void wait_process(void* param)
{
    task_struct* cur = CURRENT_TASK();
    sema_wait(&sema);
    printk("process %d triggered\n", cur->psid);
}

void test_event_process()
{

    int i = 0;
    sema_init(&sema, "test", 1);

    ps_create(main_process, 0, 1, ps_kernel);
    for (i = 0; i < TEST_LOCK; i++)
    {
        ps_create(wait_process, 0, 1, ps_kernel);
    }

    ps_kickoff();
}

#endif
#endif
