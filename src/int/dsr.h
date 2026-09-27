#ifndef _INT_DSR_H
#define _INT_DSR_H

#include <lib/list.h>

typedef void (*dsr_callback)(void *param);

typedef struct _dsr_node {
	dsr_callback fn;
	void *param;
	list_entry dsr_list;
} dsr_node;

void dsr_init();

/* Returns 1 on success, 0 if the cache was exhausted and the DSR was dropped.
 * Callers that perform hardware I/O must handle the 0 case to avoid leaving
 * the device in a state where it will not generate further interrupts. */
int dsr_add(dsr_callback fn, void *param);

/* Start the deferred-callback worker before scheduler kickoff. */
void dsr_start(void);

/* Report runnable deferred work at interrupt exit. */
int dsr_needs_schedule(void);

#endif
