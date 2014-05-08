#ifndef _DSR_H_
#define _DSR_H_

#include "list.h"

typedef void (*dsr_callback)(void* param);

typedef struct _dsr_node
{
	dsr_callback fn;
	void* param;
	LIST_ENTRY dsr_list;
}dsr_node, *dsr_node_t;

void dsr_init();

void dsr_add(dsr_callback fn, void* param);

void dsr_process();

#endif
