#ifndef _LOCK_H_
#define _LOCK_H_
#include <lib/list.h>

typedef struct _spinlock
{
	unsigned int lock;
	int inited;
	unsigned int int_status;
	unsigned holder;
}spinlock, *spinlock_t;

void spinlock_init(spinlock_t lock);

void spinlock_uninit(spinlock_t lock);

void spinlock_lock(spinlock_t lock);

void spinlock_unlock(spinlock_t lock);


// waitable object
typedef enum _wait_type
{
	wait_type_event, // wakeup only one object in waiting list
	wait_type_notify, // wakeup all object int waiting list
	wait_type_lock // wakeup one object and that object get lock
}wait_type;

typedef struct _waitable_object
{
	char name[16];
	wait_type type;
	LIST_ENTRY wait_list;
	unsigned int lock;
	unsigned int depth;
}waitable_object;

typedef struct _event
{
	waitable_object obj;
}event;

event* create_event(wait_type type, unsigned initial_state, const char* name);

void delete_event(event* e);

void wait_event(event* e);

void set_event(event* e);

// semaphore

typedef struct _semaphore
{
	char name[16];
	unsigned int lock;
}semaphore;

void sema_init(semaphore* s, const char* name, unsigned int initstat);

void sema_wait(semaphore* s);

void sema_reset(semaphore* s);

void sema_trigger(semaphore* s);

void sema_trigger_at_intr(semaphore* s);

#endif
