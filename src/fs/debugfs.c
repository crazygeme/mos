#include <timer.h>
#include <debugfs.h>
#include <fs.h>
#include <klib.h>
#include <mount.h>
#include <lock.h>
#include <rbtree.h>
#include <ps.h>

void debugfs_add_dir(const char *name, const mount_point *entry)
{
}

void debugfs_remove_dir(const char *name);

void debugfs_add_file(const char *name);

void debugfs_remove_file(const char *name);

static int debugfs_read(void *inode, void *buf, size_t count, size_t *rcnt)
{
	// 	key_value_pair *entry = hash_first(debugfs);
	// 	struct linux_dirent *prev = NULL;
	// 	struct linux_dirent *dirp = buf;
	// 	int retcount = 0;
	// 	int len;
	// 	int cur_pos = 0;

	// 	while (count > 0) {
	// 		if (entry == NULL) {
	// 			if (prev) {
	// 				prev->d_off = retcount;
	// 			}
	// 			break;
	// 		}
	// 		// entry->name[entry->name_length] = '\0';
	// 		len = ROUND_UP(NAME_OFFSET(dirp) + strlen(entry->key) + 1);
	// 		if (count < len) {
	// 			if (prev) {
	// 				prev->d_off = retcount;
	// 			}
	// 			break;
	// 		}
	// 		memset(dirp, 0, len);
	// 		dirp->d_ino = 0;
	// 		strcpy(dirp->d_name, entry->key);
	// 		dirp->d_reclen =
	// 			ROUND_UP(NAME_OFFSET(dirp) + strlen(dirp->d_name) + 1);
	// 		cur_pos += dirp->d_reclen;
	// 		dirp->d_off = cur_pos;
	// 		retcount += dirp->d_reclen;
	// 		count -= dirp->d_reclen;
	// 		prev = dirp;
	// 		dirp = (char *)dirp + dirp->d_reclen;

	// 		entry = hash_next(debugfs, entry);
	// 	}
	// done:
	// 	if (rcnt)
	// 		*rcnt = retcount;

	if (rcnt)
		*rcnt = 0;

	return 0;
}

static int debugfs_close(void *inode)
{
	return 0;
}

static int debugfs_seek(void *inode, uint64_t offset, uint32_t origin)
{
	return 0;
}

static int debugfs_select(void *inode, unsigned type)
{
	if (type == FS_SELECT_EXCEPT || type == FS_SELECT_WRITE)
		return -1;

	// can read any time
	return 0;
}

static int debugfs_stat(void *inode, struct stat *s)
{
	s->st_atime = time_now();
	s->st_mode = (S_IFDIR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR);
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now();
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

int debugfs_ioctl(void *inode, unsigned cmd, void *buf)
{
	return 0;
}

static fileop debugfsop = {
	.read = debugfs_read,
	.close = debugfs_close,
	.seek = debugfs_seek,
	.stat = debugfs_stat,
	.select = debugfs_select,
};

static filep alloc()
{
	filep fp = calloc(1, sizeof(*fp));
	fp->file_type = FILE_TYPE_CHAR;
	fp->inode = NULL;
	fp->ref_cnt = 0;
	fp->op = debugfsop;
	fp->mode = S_IFDIR;
	return fp;
}

static mount_op mp = {
	.alloc = alloc,
};

void debugfs_init()
{
	task_struct *cur = CURRENT_TASK();
	do_mount(cur->root, "/proc", &mp);
}