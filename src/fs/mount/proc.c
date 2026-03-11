
#include <fs/super/debugfs.h>
#include <fs/fs.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <lib/lock.h>
#include <lib/rbtree.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <ext4.h>

typedef struct _proc_inode {
	super_block *sb;
	unsigned offset;
	unsigned length;
	struct linux_dirent *buf;
} proc_inode;

static ssize_t proc_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	proc_inode *node = fp->f_inode->i_private;
	size_t left, read_size = 0;

	if (!node->buf || !node->length)
		goto done;

	left = node->length - node->offset;
	read_size = left > count ? count : left;
	memcpy(buf, (char *)node->buf + node->offset, read_size);
	node->offset += read_size;

done:
	*pos += read_size;
	return (ssize_t)read_size;
}

static int proc_dir_release(inode *node, file *fp)
{
	proc_inode *pi = node->i_private;
	kfree(pi->buf);
	kfree(pi);
	free(node);
	return 0;
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
	s->st_mode = (S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
		      S_IWGRP | S_IWOTH);
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

static void proc_dir_gen(super_block *sb, proc_inode *node)
{
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
	for (kv = hash_first(sb->s_files); kv;
	     kv = hash_next(sb->s_files, kv)) {
		size += ROUND_UP(NAME_OFFSET() + strlen(kv->key));
	}
	mutex_unlock(&sb->s_lock);

	/* "." and ".." entries */
	size += ROUND_UP(NAME_OFFSET() + 2);
	size += ROUND_UP(NAME_OFFSET() + 3);

	buf = kmalloc(size);
	begin = buf;
	node->offset = 0;
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
	for (kv = hash_first(sb->s_files); kv;
	     kv = hash_next(sb->s_files, kv)) {
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
	.release = proc_dir_release,
	.read = proc_dir_read,
	.llseek = proc_dir_llseek,
	.poll = proc_dir_poll,
	.ioctl = proc_dir_ioctl,
};

static inode *proc_get_root(super_block *sb)
{
	proc_inode *pi = malloc(sizeof(*pi));
	pi->sb = sb;
	proc_dir_gen(sb, pi);

	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFDIR;
	node->i_op = &proc_dir_iops;
	node->i_fop = &proc_dir_fops;
	node->i_private = pi;
	return node;
}

static super_operations proc_sops = {
	.get_root = proc_get_root,
};

static void debugfs_init()
{
	task_struct *cur = CURRENT_TASK();
	super_block *root = cur->root;
	debugfs_init_fn_t *fn;

	vfs_mount(root, "/proc", sget(&proc_sops));
	for (fn = __debugfs_init_start; fn < __debugfs_init_end; fn++)
		(*fn)(root);
}

KERNEL_INIT(5, debugfs_init);
