#include "macro.h"
#include <time.h>
#include <debugfs.h>
#include <fs.h>
#include <klib.h>
#include <mount.h>
#include <lock.h>
#include <rbtree.h>
#include <ps.h>

typedef struct _proc_inode {
	mount_point *mp;
	unsigned offset;
	unsigned length;
	struct linux_dirent *buf;
} proc_inode;

static int proc_dir_read(void *inode, void *buf, size_t count, size_t *rcnt)
{
	proc_inode *node = inode;
	unsigned left = 0;
	size_t read_size = 0;

	if (!node->buf || !node->length)
		goto done;

	left = node->length - node->offset;
	read_size = left > count ? count : left;
	memcpy(buf, (char *)node->buf + node->offset, read_size);
	node->offset += read_size;

done:
	if (rcnt)
		*rcnt = read_size;
	return 0;
}

static int proc_dir_close(void *inode)
{
	proc_inode *node = inode;
	kfree(node->buf);
	kfree(inode);
	return 0;
}

static int proc_dir_seek(void *inode, uint64_t offset, uint32_t origin)
{
	return 0;
}

static int proc_dir_select(void *inode, unsigned type)
{
	if (type == FS_SELECT_EXCEPT || type == FS_SELECT_WRITE)
		return -1;

	// can read any time
	return 0;
}

static int proc_dir_stat(void *inode, struct stat *s)
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

static int proc_dir_ioctl(void *inode, unsigned cmd, void *buf)
{
	return 0;
}

static filep proc_dir_open(void *inode, const char *path, int flag, char *mode)
{
	proc_inode *node = inode;
	mount_point *mp = node->mp;

	if (strcmp(path, "/") == 0) {
		return mp->op.alloc(mp);
	}

	return NULL;
}

static void proc_dir_gen(mount_point *mp, proc_inode *node)
{
	unsigned size = 0;
	key_value_pair *kv = NULL;
	char *buf = NULL;
	const char *begin = NULL;
	struct linux_dirent *dirp = NULL;

	mutex_lock(&mp->lock);
	for (kv = hash_first(mp->folders); kv;
	     kv = hash_next(mp->folders, kv)) {
		size += ROUND_UP(NAME_OFFSET() + strlen(kv->key));
	}

	for (kv = hash_first(mp->files); kv; kv = hash_next(mp->files, kv)) {
		size += ROUND_UP(NAME_OFFSET() + strlen(kv->key));
	}

	mutex_unlock(&mp->lock);
	// folder of .
	size += ROUND_UP(NAME_OFFSET() + 2);
	// folder of ..
	size += ROUND_UP(NAME_OFFSET() + 3);

	buf = kmalloc(size);
	begin = buf;
	node->offset = 0;
	node->length = size;
	node->buf = buf;

	// fill content
	dirp = buf;
	dirp->d_ino = DBGFS_INODE;
	strcpy(dirp->d_name, ".");
	dirp->d_reclen = ROUND_UP(NAME_OFFSET() + 2);
	dirp->d_off = buf + dirp->d_reclen - begin;
	buf += dirp->d_reclen;

	dirp = buf;
	dirp->d_ino = DBGFS_INODE;
	strcpy(dirp->d_name, "..");
	dirp->d_reclen = ROUND_UP(NAME_OFFSET() + 3);
	dirp->d_off = buf + dirp->d_reclen - begin;
	buf += dirp->d_reclen;

	mutex_lock(&mp->lock);
	for (kv = hash_first(mp->folders); kv;
	     kv = hash_next(mp->folders, kv)) {
		dirp = buf;
		dirp->d_ino = DBGFS_INODE;
		strcpy(dirp->d_name, (char *)kv->key + 1);
		dirp->d_reclen = ROUND_UP(NAME_OFFSET() + strlen(kv->key));
		dirp->d_off = buf + dirp->d_reclen - begin;
		buf += dirp->d_reclen;
	}
	for (kv = hash_first(mp->files); kv; kv = hash_next(mp->files, kv)) {
		dirp = buf;
		dirp->d_ino = DBGFS_INODE;
		strcpy(dirp->d_name, (char *)kv->key + 1);
		dirp->d_reclen = ROUND_UP(NAME_OFFSET() + strlen(kv->key));
		dirp->d_off = buf + dirp->d_reclen - begin;
		buf += dirp->d_reclen;
	}
	mutex_unlock(&mp->lock);
}

static fileop proc_dirop = {
	.open = proc_dir_open,
	.read = proc_dir_read,
	.close = proc_dir_close,
	.seek = proc_dir_seek,
	.stat = proc_dir_stat,
	.select = proc_dir_select,
};

static filep proc_dir_alloc(mount_point *mp)
{
	filep fp = calloc(1, sizeof(*fp));
	proc_inode *inode = malloc(sizeof(*inode));
	inode->mp = mp;
	proc_dir_gen(mp, inode);
	fp->inode = inode;
	fp->ref_cnt = 1;
	fp->op = proc_dirop;
	fp->mode = S_IFDIR;
	return fp;
}

static mount_op proc_dir_mp = {
	.alloc = proc_dir_alloc,
};

void debugfs_init()
{
	task_struct *cur = CURRENT_TASK();
	do_mount(cur->root, "/proc", &proc_dir_mp);
	debugfs_mm_init(cur->root);
	debugfs_ps_init(cur->root);
	debugfs_cpu_init(cur->root);
	debugfs_fs_init(cur->root);
	debugfs_sched_init(cur->root);
}