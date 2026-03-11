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

void dsr_add(dsr_callback fn, void *param);

/* Drain all pending DSR callbacks inline. Called from _task_sched. */
void dsr_drain();

#endif
