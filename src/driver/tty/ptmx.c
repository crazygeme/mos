#include <device/chardev.h>
#include <fs/entries.h>
#include <driver/driver.h>
#include <fs/fs.h>
#include <ps/ps.h>
#include <fs/mount.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <device/devnode.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <lib/command.h>
#include <errno.h>
#include <macro.h>
#include "pts_internal.h"
#include <device/devnums.h>

#define MAX_PTS 16
#define PTMX_INO 2

static spinlock_t pts_alloc_lock;
static pts_pair pts_pairs[MAX_PTS];
static unsigned pts_used;
struct devpts_instance {
	unsigned refs, mode, gid, has_gid, ptmx_mode;
};
static struct devpts_instance initial_instance = { .refs = 1,
						   .mode = 0620,
						   .ptmx_mode = 0666 };
static void instance_put(struct devpts_instance *instance)
{
	if (__sync_sub_and_fetch(&instance->refs, 1) == 0)
		free(instance);
}
static struct devpts_instance *instance_of(super_block *sb)
{
	return sb && sb->s_fs_info ? sb->s_fs_info : &initial_instance;
}
static file *ptmx_instance_open(super_block *, unsigned, int);
static file *ptmx_path_open(super_block *, int);
static void ptmx_path_close(pts_pair *p)
{
	__sync_sub_and_fetch(&p->path_count, 1);
	pts_pair_check_free(p, &pts_alloc_lock);
}

static struct rb_root pts_groups = _RBTREE_ROOT_INIT;

static void ptmx_group_insert_unsafe(pts_pair *p)
{
	struct rb_node **link = &pts_groups.rb_node, *parent = NULL;
	while (*link) {
		pts_pair *other = rb_entry(*link, pts_pair, group_node);
		parent = *link;
		link = p->pgrp < other->pgrp || (p->pgrp == other->pgrp &&
						 p->slot < other->slot) ?
			       &parent->rb_left :
			       &parent->rb_right;
	}
	rb_init_node(&p->group_node);
	rb_link_node(&p->group_node, parent, link);
	rb_insert_color(&p->group_node, &pts_groups);
	p->group_indexed = 1;
}

static void ptmx_group_changed(pts_pair *p, unsigned group)
{
	int irq;
	spinlock_lock(&pts_alloc_lock, &irq);
	if (p->group_indexed) {
		rb_erase(&p->group_node, &pts_groups);
		p->group_indexed = 0;
	}
	p->pgrp = group;
	if (p->used && p->master_open)
		ptmx_group_insert_unsafe(p);
	spinlock_unlock(&pts_alloc_lock, irq);
}

/* The pair allocator lock protects both the group index and allocation bits. */
static void ptmx_pair_free_unsafe(pts_pair *p)
{
	if (p->group_indexed) {
		rb_erase(&p->group_node, &pts_groups);
		p->group_indexed = 0;
	}
	pts_used &= ~(1U << p->slot);
	instance_put(p->devpts_instance);
	p->devpts_instance = NULL;
}

static pts_pair *ptmx_group_find_unsafe(unsigned group)
{
	struct rb_node *node = pts_groups.rb_node;
	pts_pair *match = NULL;
	while (node) {
		pts_pair *p = rb_entry(node, pts_pair, group_node);
		if (group <= p->pgrp) {
			if (group == p->pgrp)
				match = p;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return match;
}

static int ptmx_dir_getattr(file *fp, struct stat *s)
{
	(void)fp;
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFDIR | 0755;
	s->st_nlink = 2;
	s->st_ino = 1;
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_blksize = 1024;
	s->st_size = 0;
	s->st_blocks = 0;
	return 0;
}

static int ptmx_master_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(3, 1);
	s->st_rdev = MKDEV(UNIX98_PTMX_MAJOR, UNIX98_PTMX_MINOR);
	s->st_ino = PTMX_INO;
	s->st_nlink = 1;
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_blksize = 4096;
	s->st_size = 0;
	s->st_blocks = 0;
	return 0;
}

static int pts_slave_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	pts_pair *p = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = p->slave_mode;
	s->st_dev = MKDEV(0, 6);
	s->st_rdev = MKDEV(UNIX98_PTS_MAJOR, p->idx);
	s->st_ino = (uint64_t)p->idx + 3;
	s->st_nlink = 1;
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_uid = p->slave_uid;
	s->st_gid = p->slave_gid;
	s->st_blksize = 1024;
	s->st_size = 0;
	s->st_blocks = 0;
	return 0;
}

/*
 * Slave
 */

static int pts_slave_release(file *fp)
{
	pts_pair *p = fp->f_inode->i_private;

	cyb_writer_close(p->s2m);
	cyb_reader_close(p->m2s);
	if (__sync_add_and_fetch(&p->slave_count, -1) == 0) {
		pts_pair_check_free(p, &pts_alloc_lock);
	}

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const file_operations pts_slave_path_fops = {
	.getattr = pts_slave_getattr,
	.setattr = pts_slave_setattr,
	.chown = pts_slave_chown,
	.release = pts_slave_path_release,
};

static const file_operations pts_slave_fops = {
	.release = pts_slave_release,
	.getattr = pts_slave_getattr,
	.setattr = pts_slave_setattr,
	.chown = pts_slave_chown,
	.read = pts_slave_read,
	.write = pts_slave_write,
	.poll = pts_slave_poll,
	.ioctl = pts_slave_ioctl,
};

static file *ptmx_open_slave_pair(pts_pair *p, int flag)
{
	inode *node;
	file *fp;
	char path[32];

	node = zalloc(sizeof(*node));
	node->i_mode = p->slave_mode;
	node->i_private = p;

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = (flag & O_PATH) ? &pts_slave_path_fops : &pts_slave_fops;
	/* TIOCGPTPEER bypasses path-based open and still requires a proc fd path. */
	sprintf(path, "/dev/pts/%d", p->idx);
	fp->f_name = strdup(path);
	if (!(flag & O_PATH)) {
		cyb_reader_open(p->m2s);
		cyb_writer_open(p->s2m);
	}
	pts_acquire_controlling(fp, flag);
	return fp;
}

file *ptmx_slave_open(super_block *sb, const char *path, int flag)
{
	int idx;
	if (!path || *path != '/')
		return NULL;

	if (!strcmp(path, "/ptmx"))
		return (flag & O_PATH) ? ptmx_path_open(sb, flag) :
					 ptmx_instance_open(sb, 0, flag);
	for (const char *digit = path + 1; *digit; digit++)
		if (*digit < '0' || *digit > '9')
			return NULL;
	if (!path[1])
		return NULL;
	idx = atoi(path + 1);
	if (idx < 0 || idx >= MAX_PTS)
		return NULL;
	pts_pair *p = NULL;
	int irq;
	spinlock_lock(&pts_alloc_lock, &irq);
	for (unsigned slot = 0; slot < MAX_PTS; slot++) {
		pts_pair *candidate = &pts_pairs[slot];
		if (candidate->used && candidate->master_open &&
		    candidate->idx == idx &&
		    candidate->devpts_instance == instance_of(sb)) {
			p = candidate;
			break;
		}
	}
	if (!p) {
		spinlock_unlock(&pts_alloc_lock, irq);
		return NULL;
	}
	if (flag & O_PATH)
		p->path_count++;
	if (!(flag & O_PATH)) {
		__sync_add_and_fetch(&p->slave_count, 1);
		p->slave_ever_opened = 1;
	}

	spinlock_unlock(&pts_alloc_lock, irq);

	return ptmx_open_slave_pair(p, flag);
}

file *ptmx_open_controlling(task_struct *task, int flag)
{
	int irq;
	pts_pair *match;
	if (!task || !task->execution)
		return NULL;
	spinlock_lock(&pts_alloc_lock, &irq);
	match = ptmx_group_find_unsafe(task->thread->group_id);
	if (match && (flag & O_PATH))
		match->path_count++;
	if (match && !(flag & O_PATH)) {
		__sync_add_and_fetch(&match->slave_count, 1);
		match->slave_ever_opened = 1;
	}
	spinlock_unlock(&pts_alloc_lock, irq);
	return match ? ptmx_open_slave_pair(match, flag) : NULL;
}

/* Master */
static int pts_master_release(file *fp)
{
	pts_pair *p = fp->f_inode->i_private;

	cyb_writer_close(p->m2s);
	cyb_reader_close(p->s2m);

	pts_pair_close_master(p);
	pts_pair_check_free(p, &pts_alloc_lock);

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static int ptmx_master_ioctl(file *master, unsigned cmd, void *arg);

static const file_operations ptmx_master_fops = {
	.release = pts_master_release,
	.getattr = ptmx_master_getattr,
	.read = pts_master_read,
	.write = pts_master_write,
	.poll = pts_master_poll,
	.ioctl = ptmx_master_ioctl,
};

static int ptmx_open_peer(file *master, unsigned flags)
{
	file *slave;
	pts_pair *p;
	int irq, fd;

	if ((flags & O_ACCMODE) == O_ACCMODE ||
	    (flags & ~(O_ACCMODE | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)))
		return -EINVAL;

	p = master->f_inode->i_private;
	spinlock_lock(&pts_alloc_lock, &irq);
	if (!p->used || !p->master_open) {
		spinlock_unlock(&pts_alloc_lock, irq);
		return -EIO;
	}
	if (p->pt_locked) {
		spinlock_unlock(&pts_alloc_lock, irq);
		return -EIO;
	}
	__sync_add_and_fetch(&p->slave_count, 1);
	p->slave_ever_opened = 1;
	spinlock_unlock(&pts_alloc_lock, irq);

	slave = ptmx_open_slave_pair(p, flags);
	if (!slave) {
		__sync_sub_and_fetch(&p->slave_count, 1);
		return -ENOMEM;
	}
	slave->f_mode = flags & O_ACCMODE;
	slave->f_flag = flags;
	slave->f_count = 1;
	fd = fs_install_fd_unsafe(slave, flags & O_CLOEXEC);
	if (fd < 0)
		fs_put_file(slave);
	return fd;
}

static int ptmx_peer_ioctl(void *context, unsigned cmd, void *arg)
{
	(void)cmd;
	return ptmx_open_peer(context, (unsigned)(uintptr_t)arg);
}

static int ptmx_master_ioctl(file *master, unsigned cmd, void *arg)
{
	static const command_operation commands[256] = {
		[TIOCGPTPEER & 255] = { TIOCGPTPEER, ptmx_peer_ioctl },
	};
	static const command_operation *const groups[256] = {
		[(TIOCGPTPEER >> 8) & 255] = commands,
	};
	command_fn invoke = command_lookup(groups, cmd);
	return invoke ? invoke(master, cmd, arg) :
			pts_master_ioctl(master, cmd, arg);
}

/*
 * Unix98 PTY slaves are accessed via /dev/pts/N.
 * grantpt(3) calls ptsname_r() -> TIOCGPTN to get N, then stat("/dev/pts/N")
 * to verify ownership.  We provide a minimal directory superblock mounted at
 * /dev/pts with dynamically allocated slave entries and an instance-local ptmx entry.
 */

static ssize_t ptmx_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	memory_dir *rd = fp->f_inode->i_private;
	loff_t offset = *pos;
	ssize_t left, read_size = 0;

	if (!rd->buf || !rd->length)
		goto done;

	left = (ssize_t)rd->length - (ssize_t)offset;
	read_size = (ssize_t)count < left ? (ssize_t)count : left;
	if (read_size > 0)
		memcpy(buf, (char *)rd->buf + offset, read_size);
	else
		read_size = 0;
done:
	*pos = offset + read_size;
	return read_size;
}

static unsigned ptmx_dir_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return (events & FS_POLL_READ) ? FS_POLL_READ : 0;
}

static int ptmx_dir_release(file *fp)
{
	memory_dir *rd = fp->f_inode->i_private;
	kfree(rd->buf);
	free(rd);
	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const file_operations ptmx_dir_fops = {
	.release = ptmx_dir_release,
	.getattr = ptmx_dir_getattr,
	.read = ptmx_dir_read,
	.poll = ptmx_dir_poll,
};

static int ptmx_statfs(super_block *sb, struct statfs64 *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->f_type = 0x1cd1; /* DEVPTS_SUPER_MAGIC */
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = 255;
	return 0;
}

static void ptmx_dir_gen(super_block *sb, memory_dir *rd)
{
	unsigned active;
	int irq;
	char tty_buf[12];
	char *buf, *p;
	const char *begin;
	struct linux_dirent *dirp;
	(void)sb;
	/* A fixed upper bound permits one traversal of a consistent snapshot. */
	unsigned capacity =
		ROUND_UP(NAME_OFFSET() + 2) + ROUND_UP(NAME_OFFSET() + 3) +
		(MAX_PTS + 1) * ROUND_UP(NAME_OFFSET() + sizeof(tty_buf));
	spinlock_lock(&pts_alloc_lock, &irq);
	active = 0;
	for (unsigned slot = 0; slot < MAX_PTS; slot++)
		if (pts_pairs[slot].used && pts_pairs[slot].master_open &&
		    pts_pairs[slot].devpts_instance == instance_of(sb))
			active |= 1U << pts_pairs[slot].idx;
	spinlock_unlock(&pts_alloc_lock, irq);
	buf = p = kmalloc(capacity);
	begin = buf;
	memset(buf, 0, capacity);
	rd->buf = (struct linux_dirent *)buf;
	FILL_ENTRY(".", 1);
	FILL_ENTRY("..", 1);
	FILL_ENTRY("ptmx", PTMX_INO);
	while (active) {
		unsigned idx = __builtin_ctz(active);
		active &= active - 1;
		sprintf(tty_buf, "%u", idx);
		FILL_ENTRY(tty_buf, (uint64_t)idx + 3);
	}
	rd->length = p - begin;
}

static file *ptmx_dir_open_root(super_block *sb, int flag)
{
	memory_dir *rd = zalloc(sizeof(*rd));
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	ptmx_dir_gen(sb, rd);

	node->i_mode = S_IFDIR | 0755;
	node->i_private = rd;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &ptmx_dir_fops;
	return fp;
}

static void ptmx_dir_release_super(super_block *sb)
{
	instance_put(instance_of(sb));
	kfree(sb);
}

/*
 * ptmx_cdev_open — open /dev/ptmx (Unix98).
 * Dynamically allocates any free pair; use TIOCGPTN to discover the index.
 */
static file *ptmx_instance_open(super_block *dev_sb, unsigned rdev, int flag)
{
	struct devpts_instance *instance = instance_of(dev_sb);
	struct stat permission = { .st_mode = S_IFCHR | instance->ptmx_mode };
	if (!rdev) {
		int access = (flag & O_ACCMODE) == O_RDONLY ? R_OK :
			     (flag & O_ACCMODE) == O_WRONLY ? W_OK : R_OK | W_OK;
		int error = fs_check_perm(&permission, access);
		if (error) {
			current->execution->fs_open_error = error;
			return NULL;
		}
	}
	int i;
	pts_pair *p = NULL;
	int irq;

	spinlock_lock(&pts_alloc_lock, &irq);
	unsigned available = ((1U << MAX_PTS) - 1) & ~pts_used;
	if (!available) {
		spinlock_unlock(&pts_alloc_lock, irq);
		return NULL;
	}
	i = __builtin_ctz(available);
	pts_used |= 1U << i;
	p = &pts_pairs[i];

	memset(p, 0, sizeof(*p));
	p->slot = i;
	unsigned indices = 0;
	for (unsigned slot = 0; slot < MAX_PTS; slot++)
		if (pts_pairs[slot].used &&
		    pts_pairs[slot].devpts_instance == instance)
			indices |= 1U << pts_pairs[slot].idx;
	p->idx = __builtin_ctz(~indices);
	p->devpts_instance = instance;
	__sync_add_and_fetch(&instance->refs, 1);
	p->path_close = ptmx_path_close;
	p->used = 1;
	p->master_open = 1;
	p->group_changed = ptmx_group_changed;
	p->on_free = ptmx_pair_free_unsafe;
	ptmx_group_insert_unsafe(p);
	p->pt_locked = 1;
	p->slave_mode = S_IFCHR | instance->mode;
	if (current->execution) {
		p->slave_uid = current->credentials->uid;
		p->slave_gid = instance->has_gid ? instance->gid :
						   current->credentials->gid;
	}
	p->termios = tty_default_termios;
	p->winsize.ws_row = 24;
	p->winsize.ws_col = 80;
	spinlock_init(&p->lock);
	p->m2s = cyb_create_named(1);
	p->s2m = cyb_create_named(1);
	spinlock_unlock(&pts_alloc_lock, irq);
	cyb_writer_open(p->m2s);
	cyb_reader_open(p->s2m);

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_private = p;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &ptmx_master_fops;

	return fp;
}

static int ptmx_path_stat(file *fp, struct stat *st)
{
	struct devpts_instance *instance = fp->f_inode->i_private;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | instance->ptmx_mode;
	st->st_rdev = MKDEV(UNIX98_PTMX_MAJOR, UNIX98_PTMX_MINOR);
	st->st_ino = PTMX_INO;
	st->st_nlink = 1;
	return 0;
}
static int ptmx_path_mode(file *fp, uint32_t mode)
{
	struct devpts_instance *instance = fp->f_inode->i_private;
	instance->ptmx_mode = mode & 07777;
	return 0;
}
static int ptmx_path_release(file *fp)
{
	instance_put(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static const file_operations ptmx_path_fops = {
	.getattr = ptmx_path_stat,
	.setattr = ptmx_path_mode,
	.release = ptmx_path_release,
};
static file *ptmx_path_open(super_block *sb, int flags)
{
	struct devpts_instance *instance = instance_of(sb);
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	if (!fp || !node) {
		free(fp);
		free(node);
		return NULL;
	}
	__sync_add_and_fetch(&instance->refs, 1);
	node->i_mode = S_IFCHR | instance->ptmx_mode;
	node->i_private = instance;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_flag = flags;
	fp->f_fop = &ptmx_path_fops;
	return fp;
}
static file *ptmx_cdev_open(super_block *sb, unsigned rdev, int flags)
{
	(void)sb;
	/* The legacy device applies its own DAC mode to the selected instance. */
	file *metadata = vfs_open(current->fs->root, "/dev/pts/ptmx", O_PATH);
	if (!metadata)
		return NULL;
	if (metadata->f_fop != &ptmx_path_fops) {
		fs_put_file(metadata);
		return NULL;
	}
	super_block view = { .s_fs_info = metadata->f_inode->i_private };
	file *master = ptmx_instance_open(&view, rdev, flags);
	fs_put_file(metadata);
	return master;
}

static const super_operations ptmxdir_sops = {
	.open_root = ptmx_dir_open_root,
	.open = ptmx_slave_open,
	.release = ptmx_dir_release_super,
	.statfs = ptmx_statfs,
};

static super_block *ptmx_get_sb(const char *dev, const char *target, int flags,
				void *data)
{
	(void)dev;
	(void)target;
	(void)flags;
	struct devpts_instance configured = { .refs = 1, .mode = 0600 };
	int private = 0;
	char *options = data ? strdup(data) : NULL;
	if (data && !options)
		return NULL;
	for (char *part = options; part && *part;) {
		char *next = strchr(part, ',');
		if (next)
			*next++ = 0;
		if (!strcmp(part, "newinstance"))
		private = 1;
		else if (!strncmp(part, "mode=", 5) ||
			 !strncmp(part, "ptmxmode=", 9))
		{
			unsigned value = 0;
			char *digits = strchr(part, '=') + 1;
			if (!*digits) {
				free(options);
				return NULL;
			}
			for (char *p = digits; *p; p++) {
				if (*p < '0' || *p > '7' || value > 07777 / 8) {
					free(options);
					return NULL;
				}
				value = value * 8 + *p - '0';
			}
			if (part[0] == 'p')
				configured.ptmx_mode = value;
			else
				configured.mode = value;
		}
		else if (!strncmp(part, "gid=", 4))
		{
			unsigned value = 0;
			char *digits = part + 4;
			if (!*digits) {
				free(options);
				return NULL;
			}
			for (char *p = digits; *p; p++) {
				if (*p < '0' || *p > '9' ||
				    value > (0xffffffffU - (*p - '0')) / 10) {
					free(options);
					return NULL;
				}
				value = value * 10 + *p - '0';
			}
			configured.gid = value;
			configured.has_gid = 1;
		}
		else
		{
			free(options);
			return NULL;
		}
		part = next;
	}
	free(options);
	struct devpts_instance *instance = &initial_instance;
	if (private) {
		instance = malloc(sizeof(*instance));
		if (!instance)
			return NULL;
		*instance = configured;
	} else {
		__sync_add_and_fetch(&instance->refs, 1);
		if (data && *(char *)data) {
			instance->mode = configured.mode;
			instance->ptmx_mode = configured.ptmx_mode;
			instance->gid = configured.gid;
			instance->has_gid = configured.has_gid;
		}
	}
	super_block *sb = sget(&ptmxdir_sops);
	if (!sb) {
		instance_put(instance);
		return NULL;
	}
	sb->s_fs_info = instance;
	return sb;
}

static fs_type devpts_fs_type = { .name = "devpts", .get_sb = ptmx_get_sb };

static void ptmx_fs_type_init(void)
{
	spinlock_init(&pts_alloc_lock);
	printk("mnt: registered devpts file type\n");
	fs_register_type(&devpts_fs_type);
}

static void ptmx_dev_register(void)
{
	chardev_register_class(UNIX98_PTS_MAJOR, "pts");
	vfs_entry_device(devfs_entries(), "/ptmx", S_IFCHR | 0666,
			 MKDEV(UNIX98_PTMX_MAJOR, UNIX98_PTMX_MINOR), "ptmx",
			 ptmx_cdev_open);
}

static int ptmx_dev_register_probe(void)
{
	ptmx_fs_type_init();
	ptmx_dev_register();
	return 0;
}

static driver_t ptmx_dev_register_driver = {
	.name = "unix98-pty",
	.bus = DEVICE_BUS_VIRTUAL,
	.virtual_id = VDEV_PTMX,
	.probe_virtual = ptmx_dev_register_probe,
};
DRIVER_REGISTER(ptmx_dev_register_driver);
