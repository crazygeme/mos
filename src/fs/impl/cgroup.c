#include <ps/cgroup.h>
#include <ps/namespace.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <macro.h>
#include <errno.h>
#include <ext4.h>
#include <proc/impl/common/common.h>

#define CG_PIDS 0
#define CG_DEVICES 1
#define CG_RULES 128
struct device_rule {
	unsigned type, major, minor, access;
	int allow;
};
struct mos_cgroup {
	list_entry siblings, children;
	struct mos_cgroup *parent;
	unsigned controller, tasks, members, opens, removed;
	uint64_t limit, failures;
	char name[128];
	struct device_rule rules[CG_RULES];
	unsigned nrules;
};
static struct mos_cgroup roots[2];
static mutex_t cg_lock;
static int cg_ready;
static void cg_prepare(void)
{
	if (cg_ready)
		return;
	mutex_init(&cg_lock);
	for (unsigned i = 0; i < 2; i++) {
		roots[i].controller = i;
		roots[i].limit = UINT64_MAX;
		list_init(&roots[i].children);
	}
	cg_ready = 1;
}
struct cg_file {
	struct mos_cgroup *group;
	unsigned kind;
};
enum {
	CG_DIR,
	CG_PROCS,
	CG_TASKS,
	CG_NOTIFY,
	CG_CLONE,
	CG_RELEASE,
	CG_LIMIT,
	CG_CURRENT,
	CG_EVENTS,
	CG_ALLOW,
	CG_DENY,
	CG_LIST
};
static const char *const common_names[] = { "cgroup.procs", "tasks",
					    "notify_on_release",
					    "cgroup.clone_children" };
static const char *const pids_names[] = { "pids.max", "pids.current",
					  "pids.events" };
static const char *const device_names[] = { "devices.allow", "devices.deny",
					    "devices.list" };

static struct mos_cgroup *task_group(task_struct *task, unsigned controller)
{
	return task->cgroups[controller] ? task->cgroups[controller] :
					   &roots[controller];
}
static void group_path(struct mos_cgroup *group, char *out)
{
	if (!group->parent) {
		strcpy(out, "/");
		return;
	}
	group_path(group->parent, out);
	if (strcmp(out, "/"))
		strcat(out, "/");
	strcat(out, group->name);
}
void ps_cgroup_absolute_path(task_struct *task, unsigned controller, char *out)
{
	mutex_lock(&cg_lock);
	group_path(task_group(task, controller), out);
	mutex_unlock(&cg_lock);
}
void ps_cgroup_path(task_struct *task, unsigned controller, char *out)
{
	ps_cgroup_absolute_path(task, controller, out);
	const char *root = ps_namespace_cgroup_root(current, controller);
	unsigned len = strlen(root);
	if (len > 1 && !strncmp(out, root, len) &&
	    (!out[len] || out[len] == '/')) {
		if (!out[len])
			strcpy(out, "/");
		else
			memmove(out, out + len, strlen(out + len) + 1);
	}
}
static void group_maybe_free(struct mos_cgroup *group)
{
	if (!group->parent || !group->removed || group->opens || group->members)
		return;
	struct mos_cgroup *parent = group->parent;
	free(group);
	parent->opens--;
	group_maybe_free(parent);
}
void ps_cgroup_init(task_struct *task)
{
	cg_prepare();
	mutex_lock(&cg_lock);
	for (unsigned i = 0; i < 2; i++) {
		task->cgroups[i] = &roots[i];
		roots[i].tasks++;
		roots[i].members++;
	}
	mutex_unlock(&cg_lock);
}
void ps_cgroup_exit(task_struct *task)
{
	mutex_lock(&cg_lock);
	if (!task->cgroup_exited) {
		for (unsigned i = 0; i < 2; i++)
			if (task->cgroups[i] && task->cgroups[i]->tasks)
				task->cgroups[i]->tasks--;
		task->cgroup_exited = 1;
	}
	mutex_unlock(&cg_lock);
}
static unsigned group_count(struct mos_cgroup *group)
{
	unsigned n = group->tasks;
	for (list_entry *p = group->children.next; p != &group->children;
	     p = p->next)
		n += group_count(container_of(p, struct mos_cgroup, siblings));
	return n;
}
static unsigned group_subtree_count(struct mos_cgroup *group)
{
	unsigned count = 1;
	for (list_entry *n = group->children.next; n != &group->children;
	     n = n->next)
		count += group_subtree_count(
			container_of(n, struct mos_cgroup, siblings));
	return count;
}
unsigned ps_cgroup_count(unsigned controller)
{
	if (controller > 1)
		return 0;
	mutex_lock(&cg_lock);
	unsigned count = group_subtree_count(&roots[controller]);
	mutex_unlock(&cg_lock);
	return count;
}
int ps_cgroup_clone(task_struct *parent, task_struct *child)
{
	mutex_lock(&cg_lock);
	struct mos_cgroup *group = task_group(parent, CG_PIDS);
	for (struct mos_cgroup *g = group; g; g = g->parent)
		if ((uint64_t)group_count(g) >= g->limit) {
			g->failures++;
			mutex_unlock(&cg_lock);
			return -EAGAIN;
		}
	for (unsigned i = 0; i < 2; i++) {
		child->cgroups[i] = task_group(parent, i);
		child->cgroups[i]->tasks++;
		child->cgroups[i]->members++;
	}
	mutex_unlock(&cg_lock);
	return 0;
}
void ps_cgroup_put(task_struct *task)
{
	if (!cg_ready)
		return;
	mutex_lock(&cg_lock);
	for (unsigned i = 0; i < 2; i++) {
		if (task->cgroups[i]) {
			struct mos_cgroup *g = task->cgroups[i];
			if (!task->cgroup_exited && g->tasks)
				g->tasks--;
			g->members--;
			group_maybe_free(g);
		}
		task->cgroups[i] = NULL;
	}
	mutex_unlock(&cg_lock);
}
static int rule_match(struct device_rule *r, unsigned type, unsigned major,
		      unsigned minor)
{
	return (!r->type || r->type == type) &&
	       (r->major == ~0U || r->major == major) &&
	       (r->minor == ~0U || r->minor == minor);
}
int ps_cgroup_device(unsigned mode, unsigned dev, unsigned access)
{
	unsigned type = S_ISBLK(mode) ? 'b' : 'c';
	unsigned major = (dev >> 8) & 0xfff;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);
	int allowed = 1;
	mutex_lock(&cg_lock);
	for (struct mos_cgroup *g = task_group(current, CG_DEVICES); g;
	     g = g->parent) {
		unsigned permitted = 7;
		for (unsigned i = 0; i < g->nrules; i++) {
			struct device_rule *r = &g->rules[i];
			if (rule_match(r, type, major, minor)) {
				if (r->allow)
					permitted |= r->access;
				else
					permitted &= ~r->access;
			}
		}
		if ((permitted & access) != access) {
			allowed = 0;
			break;
		}
	}
	mutex_unlock(&cg_lock);
	return allowed ? 0 : -EPERM;
}
static struct mos_cgroup *group_lookup(struct mos_cgroup *root,
				       const char *path, const char **leaf)
{
	struct mos_cgroup *group = root;
	while (*path == '/')
		path++;
	while (*path) {
		const char *end = strchr(path, '/');
		unsigned length = end ? (unsigned)(end - path) : strlen(path);
		struct mos_cgroup *next = NULL;
		for (list_entry *p = group->children.next;
		     p != &group->children; p = p->next) {
			struct mos_cgroup *g =
				container_of(p, struct mos_cgroup, siblings);
			if (strlen(g->name) == length &&
			    !strncmp(g->name, path, length)) {
				next = g;
				break;
			}
		}
		if (!next) {
			*leaf = path;
			return group;
		}
		group = next;
		path += length;
		while (*path == '/')
			path++;
	}
	*leaf = path;
	return group;
}
static int cg_getattr(file *fp, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = fp->f_inode->i_mode;
	s->st_ino = fp->f_inode->i_ino;
	s->st_nlink = S_ISDIR(s->st_mode) ? 2 : 1;
	s->st_blksize = PAGE_SIZE;
	s->st_dev = 0xc9;
	return 0;
}
struct cg_enumerate {
	struct mos_cgroup *group;
	proc_buf_t *pb;
	int processes;
};
static void cg_emit_task(task_struct *task, void *opaque)
{
	struct cg_enumerate *c = opaque;
	if (task_group(task, c->group->controller) != c->group ||
	    task->life->type == ps_kernel || task->cgroup_exited ||
	    (c->processes && task->life->psid != task->thread->tgid))
		return;
	unsigned pid = ps_pid_visible(task->life->psid);
	if (pid)
		proc_buf_printf(c->pb, "%u\n", pid);
}
static void cg_dir_entry(proc_buf_t *pb, const char *name)
{
	char buffer[256];
	struct linux_dirent *d = (void *)buffer;
	unsigned length = ROUND_UP(NAME_OFFSET() + strlen(name) + 1);
	memset(buffer, 0, length);
	d->d_ino = 1;
	d->d_reclen = length;
	d->d_off = pb->len + length;
	strcpy(d->d_name, name);
	proc_buf_copy(pb, buffer, length);
}
static ssize_t cg_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	struct cg_file *f = fp->f_inode->i_private;
	struct mos_cgroup *g = f->group;
	proc_buf_t *pb = proc_buf_new();
	if (!pb)
		return -ENOMEM;
	mutex_lock(&cg_lock);
	switch (f->kind) {
	case CG_DIR:
		cg_dir_entry(pb, ".");
		cg_dir_entry(pb, "..");
		for (unsigned i = 0; i < 4; i++)
			cg_dir_entry(pb, common_names[i]);
		if (!g->parent)
			cg_dir_entry(pb, "release_agent");
		for (unsigned i = 0; i < 3; i++)
			cg_dir_entry(pb, g->controller ? device_names[i] :
							 pids_names[i]);
		for (list_entry *p = g->children.next; p != &g->children;
		     p = p->next)
			cg_dir_entry(pb, container_of(p, struct mos_cgroup,
						      siblings)
						 ->name);
		break;
	case CG_PROCS:
	case CG_TASKS: {
		struct cg_enumerate c = { g, pb, f->kind == CG_PROCS };
		ps_enum_all(cg_emit_task, &c);
		break;
	}
	case CG_LIMIT:
		if (g->limit == UINT64_MAX)
			proc_buf_printf(pb, "max\n");
		else
			proc_buf_printf(pb, "%llu\n",
					(unsigned long long)g->limit);
		break;
	case CG_CURRENT:
		proc_buf_printf(pb, "%u\n", group_count(g));
		break;
	case CG_EVENTS:
		proc_buf_printf(pb, "max %llu\n",
				(unsigned long long)g->failures);
		break;
	case CG_LIST:
		if (!g->nrules ||
		    (g->rules[0].allow && !g->rules[0].type &&
		     g->rules[0].major == ~0U && g->rules[0].minor == ~0U))
			proc_buf_printf(pb, "a *:* rwm\n");
		else
			for (unsigned i = 0; i < g->nrules; i++) {
				struct device_rule *r = &g->rules[i];
				if (!r->allow)
					continue;
				char major[20], minor[20];
				if (r->major == ~0U)
					strcpy(major, "*");
				else
					sprintf(major, "%u", r->major);
				if (r->minor == ~0U)
					strcpy(minor, "*");
				else
					sprintf(minor, "%u", r->minor);
				proc_buf_printf(pb, "%c %s:%s %s%s%s\n",
						r->type ? r->type : 'a', major,
						minor,
						(r->access & 1) ? "r" : "",
						(r->access & 2) ? "w" : "",
						(r->access & 4) ? "m" : "");
			}
		break;
	case CG_RELEASE:
		proc_buf_printf(pb, "\n");
		break;
	default:
		proc_buf_printf(pb, "0\n");
	}
	mutex_unlock(&cg_lock);
	ssize_t n = *pos >= (loff_t)pb->len ? 0 : (ssize_t)(pb->len - *pos);
	if (n > (ssize_t)size)
		n = size;
	if (n > 0) {
		memcpy(buf, pb->buf + *pos, n);
		*pos += n;
	}
	proc_buf_free(pb);
	return n;
}
static int number(const char **text, uint64_t *out)
{
	const char *p = *text;
	if (*p < '0' || *p > '9')
		return -EINVAL;
	uint64_t value = 0;
	while (*p >= '0' && *p <= '9') {
		unsigned digit = *p++ - '0';
		if (value > (UINT64_MAX - digit) / 10)
			return -EINVAL;
		value = value * 10 + digit;
	}
	*text = p;
	*out = value;
	return 0;
}
struct cg_move {
	unsigned tgid;
	struct mos_cgroup *group;
};
static void cg_move_task(task_struct *task, void *opaque)
{
	struct cg_move *c = opaque;
	if (task->thread->tgid != c->tgid || task->cgroup_exited)
		return;
	struct mos_cgroup *old = task_group(task, c->group->controller);
	if (task->cgroups[c->group->controller] && old->tasks)
		old->tasks--;
	if (task->cgroups[c->group->controller])
		old->members--;
	task->cgroups[c->group->controller] = c->group;
	c->group->tasks++;
	c->group->members++;
	group_maybe_free(old);
}
static ssize_t cg_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	struct cg_file *f = fp->f_inode->i_private;
	struct mos_cgroup *g = f->group;
	char text[256];
	const char *p = text;
	uint64_t value = 0;
	int ret = 0;
	if (!ps_has_capability(current, 21))
		return -EPERM;
	if (!size || size >= sizeof(text))
		return -EINVAL;
	memcpy(text, buf, size);
	text[size] = 0;
	mutex_lock(&cg_lock);
	if (g->removed) {
		ret = -ENOENT;
		goto done;
	}
	switch (f->kind) {
	case CG_PROCS:
	case CG_TASKS: {
		if (number(&p, &value) || value > 0x7fffffff) {
			ret = -EINVAL;
			break;
		}
		unsigned global = value ? ps_pid_global(value) :
					  current->life->psid;
		task_struct *task = ps_find_process_ref(global);
		if (!task || task->cgroup_exited) {
			if (task)
				ps_put_process_ref(task);
			ret = -ESRCH;
			break;
		}
		if (f->kind == CG_PROCS) {
			struct cg_move c = { task->thread->tgid, g };
			ps_enum_all(cg_move_task, &c);
		} else {
			struct mos_cgroup *old =
				task_group(task, g->controller);
			if (task->cgroups[g->controller] && old->tasks)
				old->tasks--;
			if (task->cgroups[g->controller])
				old->members--;
			task->cgroups[g->controller] = g;
			g->tasks++;
			g->members++;
			group_maybe_free(old);
		}
		ps_put_process_ref(task);
		break;
	}
	case CG_LIMIT:
		if (!strncmp(p, "max", 3)) {
			value = UINT64_MAX;
			p += 3;
		} else if (number(&p, &value)) {
			ret = -EINVAL;
			break;
		}
		if (*p && *p != '\n') {
			ret = -EINVAL;
			break;
		}
		g->limit = value;
		break;
	case CG_ALLOW:
	case CG_DENY: {
		struct device_rule r = { .major = ~0U,
					 .minor = ~0U,
					 .allow = f->kind == CG_ALLOW };
		if (*p != 'a' && *p != 'b' && *p != 'c') {
			ret = -EINVAL;
			break;
		}
		r.type = *p == 'a' ? 0 : *p;
		p++;
		while (*p == ' ')
			p++;
		if (*p == '*')
			p++;
		else {
			if (number(&p, &value) || value > 0xffffffff) {
				ret = -EINVAL;
				break;
			}
			r.major = value;
		}
		if (*p++ != ':') {
			ret = -EINVAL;
			break;
		}
		if (*p == '*')
			p++;
		else {
			if (number(&p, &value) || value > 0xffffffff) {
				ret = -EINVAL;
				break;
			}
			r.minor = value;
		}
		while (*p == ' ')
			p++;
		while (*p && *p != '\n') {
			if (*p == 'r')
				r.access |= 1;
			else if (*p == 'w')
				r.access |= 2;
			else if (*p == 'm')
				r.access |= 4;
			else {
				ret = -EINVAL;
				break;
			}
			p++;
		}
		if (ret || !r.access) {
			ret = -EINVAL;
			break;
		}
		if (g->nrules >= CG_RULES) {
			ret = -ENOSPC;
			break;
		}
		if (!r.type && r.major == ~0U && r.minor == ~0U &&
		    r.access == 7)
			g->nrules = 0;
		g->rules[g->nrules++] = r;
		break;
	}
	case CG_NOTIFY:
	case CG_CLONE:
		if (number(&p, &value) || value != 0)
			ret = -EOPNOTSUPP;
		break;
	case CG_RELEASE:
		if (text[0] != '\n')
			ret = -EOPNOTSUPP;
		break;
	default:
		ret = -EACCES;
	}
done:
	mutex_unlock(&cg_lock);
	if (ret)
		return ret;
	*pos += size;
	return size;
}
static int cg_release(file *fp)
{
	struct cg_file *f = fp->f_inode->i_private;
	mutex_lock(&cg_lock);
	f->group->opens--;
	group_maybe_free(f->group);
	mutex_unlock(&cg_lock);
	free(f);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static unsigned char cg_dir_type(file *fp, const char *name)
{
	struct cg_file *f = fp->f_inode->i_private;
	const char *leaf;
	mutex_lock(&cg_lock);
	group_lookup(f->group, name, &leaf);
	unsigned char type = !*leaf || !strcmp(name, ".") ||
					     !strcmp(name, "..") ?
				     S_IFDIR >> 12 :
				     S_IFREG >> 12;
	mutex_unlock(&cg_lock);
	return type;
}
static unsigned cg_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return events & (FS_POLL_READ | FS_POLL_WRITE);
}
static const file_operations cg_fops = {
	.read = cg_read,
	.write = cg_write,
	.getattr = cg_getattr,
	.release = cg_release,
	.dirent_type = cg_dir_type,
	.poll = cg_poll,
};
static file *cg_open(super_block *sb, const char *path, int flags)
{
	(void)flags;
	const char *leaf;
	unsigned kind = CG_DIR;
	mutex_lock(&cg_lock);
	struct mos_cgroup *g = group_lookup(sb->s_fs_info, path, &leaf);
	if (*leaf) {
		kind = ~0U;
		for (unsigned i = 0; i < 4; i++)
			if (!strcmp(leaf, common_names[i]))
				kind = CG_PROCS + i;
		for (unsigned i = 0; i < 3; i++)
			if (!strcmp(leaf, g->controller ? device_names[i] :
							  pids_names[i]))
				kind = (g->controller ? CG_ALLOW : CG_LIMIT) +
				       i;
		if (!g->parent && !strcmp(leaf, "release_agent"))
			kind = CG_RELEASE;
		if (kind == ~0U) {
			mutex_unlock(&cg_lock);
			return NULL;
		}
	}
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	struct cg_file *f = zalloc(sizeof(*f));
	if (!fp || !node || !f) {
		free(fp);
		free(node);
		free(f);
		mutex_unlock(&cg_lock);
		return NULL;
	}
	f->group = g;
	f->kind = kind;
	g->opens++;
	node->i_private = f;
	node->i_mode = kind == CG_DIR ?
			       S_IFDIR | 0755 :
			       S_IFREG |
				       ((kind == CG_CURRENT ||
					 kind == CG_EVENTS || kind == CG_LIST) ?
						0444 :
					(kind == CG_ALLOW || kind == CG_DENY) ?
						0200 :
						0644);
	node->i_ino = (uintptr_t)g + kind;
	fp->f_inode = node;
	fp->f_fop = &cg_fops;
	fp->f_count = 1;
	mutex_unlock(&cg_lock);
	return fp;
}
static file *cg_open_root(super_block *sb, int flags)
{
	return cg_open(sb, "", flags);
}
static int cg_mkdir(super_block *sb, const char *path, unsigned mode)
{
	(void)mode;
	if (!ps_has_capability(current, 21))
		return -EPERM;
	mutex_lock(&cg_lock);
	const char *leaf;
	struct mos_cgroup *parent = group_lookup(sb->s_fs_info, path, &leaf);
	if (!*leaf) {
		mutex_unlock(&cg_lock);
		return -EEXIST;
	}
	if (strlen(leaf) >= sizeof(parent->name) || strchr(leaf, '/') ||
	    !strcmp(leaf, ".") || !strcmp(leaf, "..")) {
		mutex_unlock(&cg_lock);
		return -EINVAL;
	}
	char *prefix = name_get();
	if (!prefix) {
		mutex_unlock(&cg_lock);
		return -ENOMEM;
	}
	group_path(parent, prefix);
	int too_long = strlen(prefix) + strlen(leaf) + 2 >= MAX_PATH;
	name_put(prefix);
	if (too_long) {
		mutex_unlock(&cg_lock);
		return -ENAMETOOLONG;
	}
	struct mos_cgroup *g = zalloc(sizeof(*g));
	if (!g) {
		mutex_unlock(&cg_lock);
		return -ENOMEM;
	}
	g->parent = parent;
	parent->opens++;
	g->controller = parent->controller;
	g->limit = UINT64_MAX;
	strcpy(g->name, leaf);
	list_init(&g->children);
	list_insert_tail(&parent->children, &g->siblings);
	mutex_unlock(&cg_lock);
	return 0;
}
static int cg_rmdir(super_block *sb, const char *path)
{
	if (!ps_has_capability(current, 21))
		return -EPERM;
	mutex_lock(&cg_lock);
	const char *leaf;
	struct mos_cgroup *g = group_lookup(sb->s_fs_info, path, &leaf);
	int ret = 0;
	if (*leaf)
		ret = -ENOENT;
	else if (!g->parent || g->tasks || !list_is_empty(&g->children))
		ret = -EBUSY;
	else {
		list_remove_entry(&g->siblings);
		g->removed = 1;
		group_maybe_free(g);
	}
	mutex_unlock(&cg_lock);
	return ret;
}
static int cg_statfs(super_block *sb, struct statfs64 *s)
{
	(void)sb;
	memset(s, 0, sizeof(*s));
	s->f_type = 0x27e0eb;
	s->f_bsize = PAGE_SIZE;
	s->f_namelen = 127;
	return 0;
}
static void cg_release_super(super_block *sb)
{
	struct mos_cgroup *g = sb->s_fs_info;
	mutex_lock(&cg_lock);
	g->opens--;
	group_maybe_free(g);
	mutex_unlock(&cg_lock);
	free(sb);
}
static const super_operations cg_sops = {
	.open_root = cg_open_root,
	.open = cg_open,
	.mkdir = cg_mkdir,
	.rmdir = cg_rmdir,
	.statfs = cg_statfs,
	.release = cg_release_super,
};
static super_block *cg_mount(const char *dev, const char *target, int flags,
			     void *data)
{
	(void)dev;
	(void)target;
	(void)flags;
	const char *options = data;
	unsigned controller;
	if (options && !strcmp(options, "pids"))
		controller = CG_PIDS;
	else if (options && !strcmp(options, "devices"))
		controller = CG_DEVICES;
	else
		return NULL;
	super_block *sb = sget(&cg_sops);
	if (sb) {
		const char *leaf;
		mutex_lock(&cg_lock);
		struct mos_cgroup *root = group_lookup(
			&roots[controller],
			ps_namespace_cgroup_root(current, controller), &leaf);
		if (*leaf) {
			mutex_unlock(&cg_lock);
			free(sb);
			return NULL;
		}
		sb->s_fs_info = root;
		root->opens++;
		mutex_unlock(&cg_lock);
		strcpy(sb->s_devname, controller ? "devices" : "pids");
	}
	return sb;
}
static fs_type cg_type = { .name = "cgroup", .get_sb = cg_mount };
static void cg_init(void)
{
	cg_prepare();
	fs_register_type(&cg_type);
}
KERNEL_INIT(2, cg_init);
