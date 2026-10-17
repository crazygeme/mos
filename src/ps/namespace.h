#ifndef MOS_PS_NAMESPACE_H
#define MOS_PS_NAMESPACE_H
#include <ps/ps.h>
#include <ps/clone.h>

enum {
	MOS_NS_MNT,
	MOS_NS_UTS,
	MOS_NS_IPC,
	MOS_NS_PID,
	MOS_NS_NET,
	MOS_NS_USER,
	MOS_NS_CGROUP,
	MOS_NS_COUNT
};
struct mos_namespace;
struct netif;
void *ps_netns_get(task_struct *task);
void *ps_netns_retain(void *ns);
void ps_netns_put(void *ns);
unsigned ps_netns_tag(void *ns);
struct netif *ps_netns_loop(void *ns);
int ps_namespace_init(task_struct *task);
int ps_namespace_clone(task_struct *parent, task_struct *child,
		       unsigned long flags);
unsigned ps_namespace_reaper(task_struct *task);
void ps_namespace_exit(task_struct *task);
void ps_namespace_unregister_pid(task_struct *task);
void ps_namespace_put(task_struct *task);
unsigned ps_namespace_id(task_struct *task, unsigned kind);
const char *ps_namespace_cgroup_root(task_struct *task, unsigned controller);
char *ps_namespace_hostname(task_struct *task);
char *ps_namespace_domainname(task_struct *task);
unsigned ps_pid_for(task_struct *observer, unsigned global);
unsigned ps_pid_visible(unsigned global);
unsigned ps_pid_global(unsigned visible);
file *ps_namespace_open(task_struct *task, const char *name);
int ps_namespace_readlink(task_struct *task, const char *name, char *buf,
			  size_t size, size_t *length);
int sys_unshare(unsigned long flags);
int sys_setns(int fd, unsigned type);
task_fs *ps_copy_fs(task_fs *old);
super_block *ps_namespace_mqueue_root(void);
void ps_namespace_mount_root(super_block *root);
#endif
