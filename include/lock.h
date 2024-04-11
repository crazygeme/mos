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

typedef volatile struct _cond {
	char name[16];
	unsigned int lock;
	list_entry wait_list;
	spinlock_t wait_lock;
} cond_t;

void cond_init(cond_t *s, const char *name, unsigned int initstat);

void cond_wait(cond_t *s);

void cond_wait_at_intr(cond_t *s);

void cond_reset(cond_t *s);

void cond_notify(cond_t *s);

void cond_notify_at_intr(cond_t *s);

#endif
