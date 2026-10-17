#include <ps/namespace.h>
#include <net/net.h>
#include <errno.h>
#include <ps/cgroup.h>
#include <fs/fcntl.h>
#include <fs/entries.h>
#include <macro.h>
#include <lib/klib.h>

struct ns_pid {
	list_entry list;
	unsigned global, local;
};
struct mos_namespace {
	ref_count_t ref;
	unsigned id, kind, next_pid, init_pid, dead;
	struct mos_namespace *parent;
	spinlock_t lock;
	list_entry pids;
	super_block *root;
	struct netif *network_loop;
	unsigned network_tag;
	super_block *mqueue_root;
	rmutex_t mqueue_lock;
	char hostname[65], domainname[65];
	char *cgroup_roots[4];
};
static unsigned next_namespace_id = 0xf0000000;
static struct mos_namespace *initial[MOS_NS_COUNT];
static const char *const ns_names[] = { "mnt", "uts",  "ipc",	"pid",
					"net", "user", "cgroup" };
static const unsigned ns_flags[] = { CLONE_NEWNS,    CLONE_NEWUTS,
				     CLONE_NEWIPC,   CLONE_NEWPID,
				     CLONE_NEWNET,   CLONE_NEWUSER,
				     CLONE_NEWCGROUP };
#define NS_SUPPORTED                                                \
	(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | \
	 CLONE_NEWCGROUP | CLONE_NEWNET)

static void ns_release(ref_count_t *ref)
{
	struct mos_namespace *ns = container_of(ref, struct mos_namespace, ref);
	while (!list_is_empty(&ns->pids))
		free(container_of(list_remove_head(&ns->pids), struct ns_pid,
				  list));
	if (ns->parent)
		ref_count_put(ns->parent);
	if (ns->root) {
		vfs_namespace_untrack(ns);
		sb_put(ns->root);
	}
	if (ns->mqueue_root)
		sb_put(ns->mqueue_root);
	if (ns->network_loop)
		net_namespace_destroy(ns->network_loop);
	free(ns->cgroup_roots[0]);
	free(ns->cgroup_roots[1]);
	free(ns->cgroup_roots[2]);
	free(ns->cgroup_roots[3]);
	free(ns);
}
static struct mos_namespace *ns_alloc(unsigned kind, struct mos_namespace *old)
{
	struct mos_namespace *ns = zalloc(sizeof(*ns));
	if (!ns)
		return NULL;
	ref_count_init(&ns->ref, ns_release);
	ns->id = __sync_add_and_fetch(&next_namespace_id, 1);
	if (kind == MOS_NS_USER && !old)
		ns->id = 0xeffffffdU;
	ns->kind = kind;
	ns->next_pid = 1;
	spinlock_init(&ns->lock);
	vm_lock_init(&ns->mqueue_lock);
	list_init(&ns->pids);
	strcpy(ns->hostname, old ? old->hostname : "qemu-mos");
	strcpy(ns->domainname, old ? old->domainname : "(none)");
	if (kind == MOS_NS_CGROUP) {
		char *path = name_get();
		if (!path) {
			ref_count_put(ns);
			return NULL;
		}
		for (unsigned i = 0; i < 4; i++) {
			if (old)
				ps_cgroup_absolute_path(current, i, path);
			else
				strcpy(path, "/");
			ns->cgroup_roots[i] = strdup(path);
			if (!ns->cgroup_roots[i]) {
				name_put(path);
				ref_count_put(ns);
				return NULL;
			}
		}
		name_put(path);
	}
	if (kind == MOS_NS_NET && old) {
		ns->network_tag = ns->id;
		ns->network_loop = net_namespace_create(ns->network_tag);
		if (!ns->network_loop) {
			ref_count_put(ns);
			return NULL;
		}
	}
	if (kind == MOS_NS_PID && old)
		ns->parent = ref_count_get(old);
	return ns;
}
int ps_namespace_init(task_struct *task)
{
	for (unsigned i = 0; i < MOS_NS_COUNT; i++) {
		if (!initial[i])
			initial[i] = ns_alloc(i, NULL);
		if (!initial[i])
			return -ENOMEM;
		task->namespaces[i] = ref_count_get(initial[i]);
	}
	return 0;
}
void ps_namespace_put(task_struct *task)
{
	for (unsigned i = 0; i < MOS_NS_COUNT; i++) {
		if (task->namespaces[i])
			ref_count_put(task->namespaces[i]);
		task->namespaces[i] = NULL;
	}
	if (task->pid_children)
		ref_count_put(task->pid_children);
	task->pid_children = NULL;
}
void *ps_netns_get(task_struct *task)
{
	return task->namespaces[MOS_NS_NET] ?
		       ref_count_get(task->namespaces[MOS_NS_NET]) :
		       NULL;
}
void *ps_netns_retain(void *ns)
{
	return ns ? ref_count_get((struct mos_namespace *)ns) : NULL;
}
void ps_netns_put(void *ns)
{
	if (ns)
		ref_count_put((struct mos_namespace *)ns);
}
unsigned ps_netns_tag(void *ns)
{
	return ns ? ((struct mos_namespace *)ns)->network_tag : 0;
}
struct netif *ps_netns_loop(void *ns)
{
	return ns ? ((struct mos_namespace *)ns)->network_loop : NULL;
}
unsigned ps_namespace_id(task_struct *task, unsigned kind)
{
	return task->namespaces[kind] ? task->namespaces[kind]->id : 0;
}
const char *ps_namespace_cgroup_root(task_struct *task, unsigned controller)
{
	struct mos_namespace *ns = task->namespaces[MOS_NS_CGROUP];
	return ns && ns->cgroup_roots[controller] ?
		       ns->cgroup_roots[controller] :
		       "/";
}
char *ps_namespace_hostname(task_struct *task)
{
	return task->namespaces[MOS_NS_UTS]->hostname;
}
char *ps_namespace_domainname(task_struct *task)
{
	return task->namespaces[MOS_NS_UTS]->domainname;
}
static int ns_pid_add(struct mos_namespace *ns, unsigned global)
{
	if (!ns || ns == initial[MOS_NS_PID])
		return 0;
	struct ns_pid *entry = zalloc(sizeof(*entry));
	if (!entry)
		return -ENOMEM;
	int irq;
	entry->global = global;
	spinlock_lock(&ns->lock, &irq);
	if (ns->dead) {
		spinlock_unlock(&ns->lock, irq);
		free(entry);
		return -ENOMEM;
	}
	entry->local = ns->next_pid++;
	if (entry->local == 1)
		ns->init_pid = global;
	list_insert_tail(&ns->pids, &entry->list);
	spinlock_unlock(&ns->lock, irq);
	return 0;
}
void ps_namespace_unregister_pid(task_struct *task)
{
	for (struct mos_namespace *ns = task->namespaces[MOS_NS_PID]; ns;
	     ns = ns->parent) {
		int irq;
		spinlock_lock(&ns->lock, &irq);
		for (list_entry *n = ns->pids.next; n != &ns->pids;) {
			struct ns_pid *p = container_of(n, struct ns_pid, list);
			n = n->next;
			if (p->global == task->life->psid) {
				list_remove_entry(&p->list);
				free(p);
				break;
			}
		}
		spinlock_unlock(&ns->lock, irq);
	}
}
unsigned ps_namespace_reaper(task_struct *task)
{
	struct mos_namespace *ns = task->namespaces[MOS_NS_PID];
	return ns && ns != initial[MOS_NS_PID] && !ns->dead ? ns->init_pid : 1;
}
static void ns_kill_member(task_struct *task, void *opaque)
{
	struct mos_namespace *dying = opaque;
	if (task->life->type != ps_user ||
	    task->life->psid != task->thread->tgid ||
	    task->thread == current->thread)
		return;
	for (struct mos_namespace *ns = task->namespaces[MOS_NS_PID]; ns;
	     ns = ns->parent)
		if (ns == dying) {
			ps_send_signal(task->thread->tgid, SIGKILL);
			return;
		}
}
void ps_namespace_exit(task_struct *task)
{
	struct mos_namespace *ns = task->namespaces[MOS_NS_PID];
	if (!ns || ns == initial[MOS_NS_PID] ||
	    ns->init_pid != task->thread->tgid || task->thread->live_tasks > 1)
		return;
	ns->dead = 1;
	ps_enum_all(ns_kill_member, ns);
}
unsigned ps_pid_for(task_struct *observer, unsigned global)
{
	struct mos_namespace *ns = observer->namespaces[MOS_NS_PID];
	if (!ns || ns == initial[MOS_NS_PID])
		return global;
	unsigned result = 0;
	int irq;
	spinlock_lock(&ns->lock, &irq);
	for (list_entry *n = ns->pids.next; n != &ns->pids; n = n->next) {
		struct ns_pid *p = container_of(n, struct ns_pid, list);
		if (p->global == global) {
			result = p->local;
			break;
		}
	}
	spinlock_unlock(&ns->lock, irq);
	return result;
}
unsigned ps_pid_visible(unsigned global)
{
	return ps_pid_for(current, global);
}
unsigned ps_pid_global(unsigned local)
{
	struct mos_namespace *ns = current->namespaces[MOS_NS_PID];
	if (!ns || ns == initial[MOS_NS_PID])
		return local;
	unsigned result = 0xffffffff;
	int irq;
	spinlock_lock(&ns->lock, &irq);
	for (list_entry *n = ns->pids.next; n != &ns->pids; n = n->next) {
		struct ns_pid *p = container_of(n, struct ns_pid, list);
		if (p->local == local) {
			result = p->global;
			break;
		}
	}
	spinlock_unlock(&ns->lock, irq);
	return result;
}
static int ns_private_mount(task_struct *task, struct mos_namespace *ns)
{
	if (!initial[MOS_NS_MNT]->root) {
		initial[MOS_NS_MNT]->root = task->fs->root;
		sb_get(task->fs->root);
		vfs_namespace_track(initial[MOS_NS_MNT], task->fs->root);
	}
	task_fs *fs = ps_copy_fs(task->fs);
	if (!fs)
		return -ENOMEM;
	super_block *root = vfs_clone_mount_tree(fs->root);
	if (!root) {
		ref_count_put(fs);
		return -ENOMEM;
	}
	sb_put(fs->root);
	fs->root = root;
	ns->root = root;
	sb_get(root);
	vfs_namespace_track(ns, root);
	ref_count_put(task->fs);
	task->fs = fs;
	return 0;
}
int ps_namespace_clone(task_struct *parent, task_struct *child,
		       unsigned long flags)
{
	if (flags & CLONE_NAMESPACE_FLAGS & ~NS_SUPPORTED)
		return -EINVAL;
	for (unsigned i = 0; i < MOS_NS_COUNT; i++) {
		struct mos_namespace *old = parent->namespaces[i];
		if (i == MOS_NS_PID && parent->pid_children &&
		    !(flags & CLONE_THREAD))
			old = parent->pid_children;
		if (flags & ns_flags[i]) {
			child->namespaces[i] = ns_alloc(i, old);
			if (!child->namespaces[i])
				return -ENOMEM;
			if (i == MOS_NS_MNT) {
				int ret = ns_private_mount(
					child, child->namespaces[i]);
				if (ret)
					return ret;
			}
		} else
			child->namespaces[i] = ref_count_get(old);
	}
	if ((flags & CLONE_THREAD) && parent->pid_children)
		child->pid_children = ref_count_get(parent->pid_children);
	for (struct mos_namespace *ns = child->namespaces[MOS_NS_PID]; ns;
	     ns = ns->parent) {
		int ret = ns_pid_add(ns, child->life->psid);
		if (ret)
			return ret;
	}
	return 0;
}
int sys_unshare(unsigned long flags)
{
	if (flags & ~(NS_SUPPORTED | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM))
		return -EINVAL;
	if ((flags & CLONE_NAMESPACE_FLAGS) && !ps_has_capability(current, 21))
		return -EPERM;
	if ((flags & CLONE_FILES) && ps_unshare_fds(current))
		return -ENOMEM;
	if ((flags & CLONE_FS) && !(flags & CLONE_NEWNS)) {
		task_fs *fs = ps_copy_fs(current->fs);
		if (!fs)
			return -ENOMEM;
		ref_count_put(current->fs);
		current->fs = fs;
	}
	for (unsigned i = 0; i < MOS_NS_COUNT; i++) {
		if (!(flags & ns_flags[i]))
			continue;
		struct mos_namespace *ns = ns_alloc(i, current->namespaces[i]);
		if (!ns)
			return -ENOMEM;
		if (i == MOS_NS_MNT) {
			int ret = ns_private_mount(current, ns);
			if (ret) {
				ref_count_put(ns);
				return ret;
			}
		}
		if (i == MOS_NS_PID) {
			if (current->pid_children)
				ref_count_put(current->pid_children);
			current->pid_children = ns;
		} else {
			ref_count_put(current->namespaces[i]);
			current->namespaces[i] = ns;
		}
	}
	return 0;
}
static int ns_file_release(file *fp)
{
	ref_count_put(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static int ns_file_stat(file *fp, struct stat *s)
{
	struct mos_namespace *ns = fp->f_inode->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFREG | 0444;
	s->st_dev = 0x6e73;
	s->st_ino = ns->id;
	s->st_nlink = 1;
	return 0;
}
static file *ns_file_create(struct mos_namespace *ns);
static int ns_file_ioctl(file *fp, unsigned cmd, void *arg)
{
	struct mos_namespace *ns = fp->f_inode->i_private;
	if (cmd == 0xb701 || cmd == 0xb702) {
		struct mos_namespace *target;
		if (cmd == 0xb701) {
			if (ns->kind == MOS_NS_USER)
				return -EPERM;
			target = initial[MOS_NS_USER];
		} else {
			if (ns->kind != MOS_NS_PID && ns->kind != MOS_NS_USER)
				return -EINVAL;
			if (!ns->parent)
				return -EPERM;
			target = ns->parent;
		}
		file *opened = ns_file_create(target);
		if (!opened)
			return -ENOMEM;
		int fd = fs_install_fd(opened, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			fs_put_file(opened);
		return fd;
	}
	if (cmd == 0xb704) {
		if (ns->kind != MOS_NS_USER)
			return -EINVAL;
		if (!arg)
			return -EFAULT;
		*(unsigned *)arg = 0;
		return 0;
	}
	if (cmd == 0xb703) /* NS_GET_NSTYPE */
		return ns_flags[ns->kind];
	return -ENOTTY;
}
static const file_operations ns_fops;
static file *ns_file_reopen(file *old, int flags)
{
	(void)flags;
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	if (!fp || !node) {
		free(fp);
		free(node);
		return NULL;
	}
	*node = (inode){ .i_mode = S_IFREG | 0444,
			 .i_ino = old->f_inode->i_ino,
			 .i_private = ref_count_get(old->f_inode->i_private) };
	fp->f_inode = node;
	fp->f_fop = &ns_fops;
	fp->f_count = 1;
	return fp;
}
static const file_operations ns_fops = {
	.release = ns_file_release,
	.getattr = ns_file_stat,
	.ioctl = ns_file_ioctl,
	.reopen = ns_file_reopen,
};
static int ns_kind(const char *name)
{
	for (unsigned i = 0; i < MOS_NS_COUNT; i++)
		if (!strcmp(name, ns_names[i]))
			return i;
	return -1;
}
file *ps_namespace_open(task_struct *task, const char *name)
{
	int kind = ns_kind(name);
	if (kind < 0 || !ps_may_access_process(task))
		return NULL;
	return ns_file_create(task->namespaces[kind]);
}
static file *ns_file_create(struct mos_namespace *ns)
{
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	if (!fp || !node) {
		free(fp);
		free(node);
		return NULL;
	}
	node->i_mode = S_IFREG | 0444;
	node->i_ino = ns->id;
	node->i_private = ref_count_get(ns);
	fp->f_inode = node;
	fp->f_fop = &ns_fops;
	fp->f_count = 1;
	return fp;
}
int ps_namespace_readlink(task_struct *task, const char *name, char *buf,
			  size_t size, size_t *length)
{
	int kind = ns_kind(name);
	char target[64];
	if (kind < 0)
		return -ENOENT;
	unsigned n = sprintf(target, "%s:[%u]", ns_names[kind],
			     ps_namespace_id(task, kind));
	if (n > size)
		n = size;
	memcpy(buf, target, n);
	*length = n;
	return 0;
}
int sys_setns(int fd, unsigned type)
{
	if (!ps_has_capability(current, 21))
		return -EPERM;
	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;
	mutex_lock(&current->files->lock);
	file *fp = current->files->fds[fd];
	if (!fp || fp->f_fop != &ns_fops) {
		mutex_unlock(&current->files->lock);
		return -EINVAL;
	}
	struct mos_namespace *ns = ref_count_get(fp->f_inode->i_private);
	mutex_unlock(&current->files->lock);
	if ((type && type != ns_flags[ns->kind]) ||
	    (!(NS_SUPPORTED & ns_flags[ns->kind]) &&
	     !(ns->kind == MOS_NS_NET &&
	       ns == current->namespaces[MOS_NS_NET]))) {
		ref_count_put(ns);
		return -EINVAL;
	}
	if (ns->kind == MOS_NS_MNT && ns->root) {
		task_fs *fs = ps_copy_fs(current->fs);
		if (!fs) {
			ref_count_put(ns);
			return -ENOMEM;
		}
		sb_put(fs->root);
		fs->root = ns->root;
		sb_get(fs->root);
		ref_count_put(current->fs);
		current->fs = fs;
	}
	if (ns->kind == MOS_NS_PID) {
		struct mos_namespace *parent = ns;
		while (parent && parent != current->namespaces[MOS_NS_PID])
			parent = parent->parent;
		if (!parent) {
			ref_count_put(ns);
			return -EINVAL;
		}
		if (current->pid_children)
			ref_count_put(current->pid_children);
		current->pid_children = ns;
	} else {
		ref_count_put(current->namespaces[ns->kind]);
		current->namespaces[ns->kind] = ns;
	}
	return 0;
}

/* The IPC namespace retains the mqueue filesystem root across mount views. */
super_block *ps_namespace_mqueue_root(void)
{
	struct mos_namespace *ns = current->namespaces[MOS_NS_IPC];
	if (!ns)
		return NULL;
	LOCK_GUARD(&ns->mqueue_lock);
	if (!ns->mqueue_root) {
		vfs_entry_tree *tree = vfs_entry_tree_create();
		if (!tree)
			return NULL;
		vfs_entry_tree_type(tree, 0x19800202);
		vfs_entry_set_mode(vfs_entry_root(tree), S_IFDIR | 01777);
		ns->mqueue_root = vfs_entry_tree_super(tree);
	}
	return vfs_clone_mount_tree(ns->mqueue_root);
}

void ps_namespace_mount_root(super_block *root)
{
	struct mos_namespace *ns = current->namespaces[MOS_NS_MNT];
	if (!ns)
		return;
	super_block *old = ns->root;
	ns->root = root;
	sb_get(root);
	vfs_namespace_track(ns, root);
	if (old)
		sb_put(old);
}
