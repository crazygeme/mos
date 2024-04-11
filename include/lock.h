#ifndef _LOCK_H_
#define _LOCK_H_
#include <list.h>

typedef volatile struct _spinlock {
	unsigned int lock;
	int inited;
	unsigned int int_status;
	unsigned holder;
	int disable_intr;
} spinlock_t;

void spinlock_init(spinlock_t *lock);

void spinlock_init_ex(spinlock_t *lock, int disable_intr);

void spinlock_uninit(spinlock_t *lock);

void spinlock_lock(spinlock_t *lock);

void spinlock_unlock(spinlock_t *lock);

// cond_t

typedef struct _lock_base {
	unsigned int lock;
	list_entry wait_list;
	spinlock_t wait_lock;
} lock_base;

typedef volatile struct _cond {
	lock_base base;
	char name[16];
} cond_t;

void cond_init(cond_t *s, const char *name, unsigned int initstat);

void cond_wait(cond_t *s);

void cond_wait_at_intr(cond_t *s);

void cond_reset(cond_t *s);

void cond_notify(cond_t *s);

void cond_notify_at_intr(cond_t *s);

typedef volatile struct _mutex {
	lock_base base;
	unsigned holder;
} mutex_t;

void mutex_init(mutex_t *m);

void mutex_lock(mutex_t *m);

void mutex_unlock(mutex_t *m);

#endif
