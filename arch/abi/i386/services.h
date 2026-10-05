#ifndef MOS_I386_SERVICES_H
#define MOS_I386_SERVICES_H
#include <arch/types.h>
#include <stddef.h>
int i386_set_robust_list(void *, size_t);
int i386_get_robust_list(int, void *, void *);
int i386_shmctl(int, int, void *);
int i386_ptrace(int, int, void *, void *);
#endif
