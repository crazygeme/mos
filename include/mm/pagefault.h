#ifndef _MM_PAGEFAULT_H
#define _MM_PAGEFAULT_H

typedef struct _task_struct task_struct;

void pf_init();

void pf_enable();

void pf_disable();

int pf_resolve_task_page_fault(task_struct *task, unsigned addr, int write);

#endif
