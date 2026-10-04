#ifndef MOS_SYSCALL_INTERNAL_H
#define MOS_SYSCALL_INTERNAL_H

#include <syscall/syscall.h>

/* Path resolution shared by syscall implementation units. */
int syscall_resolve_at(int dirfd, const char *path, char *name);

#endif
