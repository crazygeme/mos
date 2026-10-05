#ifndef _NET_CORE_H
#define _NET_CORE_H

#include <ps/ps.h>

/*
 * lwIP NO_SYS calls execute under the kernel lock without task preemption.
 * Interrupt handlers may enqueue received frames but must not enter lwIP.
 * Socket operations may explicitly wait between completed lwIP calls; a
 * callback inside lwIP must not yield. The scheduling level belongs to the
 * current task, so a waiting socket does not inhibit its peers.
 */
void net_service_update(void);
void net_service_tick(void);

static inline int net_core_enter(void)
{
	if (!ps_enabled())
		return 0;

	sched_disable();
	return 1;
}

typedef struct {
	int active;
	int service;
} net_core_scope;

static inline void net_core_leave(net_core_scope *scope)
{
	if (scope->service)
		net_service_update();
	if (scope->active)
		sched_enable();
}

/* Restore the scheduling level on every scope exit, including early returns. */
/* Local sockets retain preemption protection without refreshing lwIP timers. */
#define NET_CORE_GUARD_IF(needs_service)                                     \
	net_core_scope net_core_guard                                        \
		__attribute__((cleanup(net_core_leave), unused)) = {         \
			net_core_enter(), !!(needs_service)                  \
		}
#define NET_CORE_GUARD NET_CORE_GUARD_IF(1)

#endif
