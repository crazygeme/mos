#ifndef _PROC_PROC_H
#define _PROC_PROC_H

#include <fs/entries.h>

#define PROC_INODE 0x80
file *proc_pid_lookup(unsigned pid, const char *rest, int flag);
task_struct *proc_resolve_thread(task_struct *task, const char **rest);

/* Providers register once; every procfs mount creates a view of this tree. */
vfs_entry_node *procfs_entries(void);
#endif
