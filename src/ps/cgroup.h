#ifndef MOS_PS_CGROUP_H
#define MOS_PS_CGROUP_H
#include <ps/ps.h>
unsigned ps_cgroup_count(unsigned controller);
void ps_cgroup_init(task_struct *task);
void ps_cgroup_exit(task_struct *task);
int ps_cgroup_clone(task_struct *parent, task_struct *child);
void ps_cgroup_put(task_struct *task);
int ps_cgroup_device(unsigned mode, unsigned dev, unsigned access);
void ps_cgroup_absolute_path(task_struct *task, unsigned controller,
			     char *buffer);
void ps_cgroup_path(task_struct *task, unsigned controller, char *buffer);
#endif
