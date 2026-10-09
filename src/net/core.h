#ifndef _NET_CORE_H
#define _NET_CORE_H

#include <ps/ps.h>
#include <lib/lock.h>

/*
 * A task-owned mutex serializes socket state and lwIP NO_SYS operations.
 * Interrupt handlers enqueue frames without entering lwIP. Explicit socket
 * waits suspend core ownership; lwIP callbacks must not yield.
 */
void net_service_update(void);
void net_service_tick(void);
int net_core_enter(void);
void net_core_unlock(void);
unsigned net_core_suspend(void);
void net_core_resume(unsigned depth);

/* Both policies acquire core ownership; only net_core_lock refreshes lwIP. */
extern const scoped_lock_t net_core_lock;
extern const scoped_lock_t net_local_lock;

#endif
