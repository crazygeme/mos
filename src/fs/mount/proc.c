
#include <fs/fs.h>
#include <fs/mount.h>
#include <fs/mount/proc.h>
#include <ps/ps.h>
#include <lib/lock.h>
#include <lib/rbtree.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <ext4.h>

typedef struct _proc_dir {
	inode inode;
	unsigned length;
	struct linux_dirent *buf;
} proc_dir;

static ssize_t proc_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	proc_dir *node = fp->f_inode->i_private;
	size_t left, read_size = 0;
	loff_t offset = *pos;

	if (!node->buf || !node->length)
		goto done;

	left = node->length - offset;
	read_size = left > count ? count : left;
	memcpy(buf, (char *)node->buf + offset, read_size);

done:
	*pos = offset + read_size;
	return (ssize_t)read_size;
}

static loff_t proc_dir_llseek(file *fp, loff_t offset, int whence)
{
	return 0;
}

static int proc_dir_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT || type == FS_POLL_WRITE)
		return -1;
	return 0;
}

static int proc_dir_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0xb;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_nlink = 1;
	s->st_rdev = 8004;
	s->st_size = 0;
	return 0;
}

static int proc_dir_ioctl(file *fp, unsigned cmd, void *buf)
{
	return 0;
}

static void proc_dir_gen(super_block *sb)
{
	proc_dir *node = sb->s_fs_info;
	unsigned size = 0;
	key_value_pair *kv = NULL;
	char *buf = NULL;
	const char *begin = NULL;
	struct linux_dirent *dirp = NULL;

	mutex_lock(&sb->s_lock);
	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv)) {
		size += ROUND_UP(NAME_OFFSET() + strlen(kv->key));
	}
	mutex_unlock(&sb->s_lock);

	/* "." and ".." entries */
	size += ROUND_UP(NAME_OFFSET() + 2);
	size += ROUND_UP(NAME_OFFSET() + 3);

	buf = kmalloc(size);
	begin = buf;
	node->length = size;
	node->buf = (struct linux_dirent *)buf;

	dirp = (struct linux_dirent *)buf;
	dirp->d_ino = DBGFS_INODE;
	strcpy(dirp->d_name, ".");
	dirp->d_reclen = ROUND_UP(NAME_OFFSET() + 2);
	dirp->d_off = buf + dirp->d_reclen - begin;
	buf += dirp->d_reclen;

	dirp = (struct linux_dirent *)buf;
	dirp->d_ino = DBGFS_INODE;
	strcpy(dirp->d_name, "..");
	dirp->d_reclen = ROUND_UP(NAME_OFFSET() + 3);
	dirp->d_off = buf + dirp->d_reclen - begin;
	buf += dirp->d_reclen;

	mutex_lock(&sb->s_lock);
	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv)) {
		dirp = (struct linux_dirent *)buf;
		dirp->d_ino = DBGFS_INODE;
		strcpy(dirp->d_name, (char *)kv->key + 1);
		dirp->d_reclen = ROUND_UP(NAME_OFFSET() + strlen(kv->key));
		dirp->d_off = buf + dirp->d_reclen - begin;
		buf += dirp->d_reclen;
	}
	mutex_unlock(&sb->s_lock);
}

static const inode_operations proc_dir_iops = {
	.getattr = proc_dir_getattr,
};

static const file_operations proc_dir_fops = {
	.read = proc_dir_read,
	.llseek = proc_dir_llseek,
	.poll = proc_dir_poll,
	.ioctl = proc_dir_ioctl,
};

static file *proc_open_root(super_block *sb)
{
	proc_dir *pi = sb->s_fs_info;
	proc_dir_gen(sb);

	inode *node = calloc(1, sizeof(*node));
	node->i_mode = (S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
			S_IWGRP | S_IWOTH);
	node->i_op = &proc_dir_iops;
	node->i_private = pi;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &proc_dir_fops;
	return fp;
}

static void proc_release_super(super_block *sb)
{
	proc_dir *pi = sb->s_fs_info;
	kfree(pi->buf);
	kfree(pi);
	kfree(sb);
}

static super_operations proc_sops = {
	.open_root = proc_open_root,
	.release = proc_release_super,
};

static void debugfs_init()
{
	task_struct *cur = CURRENT_TASK();
	super_block *root = cur->root;
	debugfs_init_fn_t *fn;
	super_block *sb = sget(&proc_sops);
	proc_dir *pi = kmalloc(sizeof(*pi));

	sb->s_fs_info = pi;

	vfs_mount(root, "/proc", sb);

	for (fn = __debugfs_init_start; fn < __debugfs_init_end; fn++)
		(*fn)(sb);
}

KERNEL_INIT(5, debugfs_init);
