#ifndef _LOCK_H_
#define _LOCK_H_
#include <list.h>

typedef struct _spinlock {
    unsigned int lock;
    int inited;
    unsigned int int_status;
    unsigned holder;
} spinlock, *spinlock_t;

void spinlock_init(spinlock_t lock);

void spinlock_uninit(spinlock_t lock);

void spinlock_lock(spinlock_t lock);

void spinlock_unlock(spinlock_t lock);

// cond_t

typedef struct _cond {
    char name[16];
    unsigned int lock;
    list_entry wait_list;
    spinlock wait_lock;
} cond_t;

void cond_init(cond_t* s, const char* name, unsigned int initstat);

void cond_wait(cond_t* s);

void cond_wait_at_intr(cond_t* s);

void cond_reset(cond_t* s);

void cond_notify(cond_t* s);

void cond_notify_at_intr(cond_t* s);

#endif
