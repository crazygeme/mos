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
static inline int net_core_enter(void)
{
	if (!ps_enabled())
		return 0;

	sched_disable();
	return 1;
}

static inline void net_core_leave(int *active)
{
	if (*active)
		sched_enable();
}

/* Restore the scheduling level on every scope exit, including early returns. */
#define NET_CORE_GUARD \
	int net_core_guard __attribute__((cleanup(net_core_leave), unused)) = \
		net_core_enter()

#endif
