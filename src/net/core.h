#ifndef _NET_CORE_H
#define _NET_CORE_H

#include <ps/ps.h>

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

typedef struct {
	int active;
	int service;
} net_core_scope;

static inline void net_core_leave(net_core_scope *scope)
{
	if (scope->service)
		net_service_update();
	if (scope->active)
		net_core_unlock();
}

/* Restore the scheduling level on every scope exit, including early returns. */
/* Local sockets share core ownership without refreshing lwIP timers. */
#define NET_CORE_GUARD_IF(needs_service)                             \
	net_core_scope net_core_guard                                \
		__attribute__((cleanup(net_core_leave), unused)) = { \
			net_core_enter(), !!(needs_service)          \
		}
#define NET_CORE_GUARD NET_CORE_GUARD_IF(1)

#endif
