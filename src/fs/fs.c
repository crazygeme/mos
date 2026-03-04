#include "lock.h"
#include <mount.h>
#include <unistd.h>
#include <fs.h>
#include <block.h>
#include <klib.h>
#include <ps.h>
#include <lwext4/include/ext4.h>
#include <fcntl.h>
#include <include/fs.h>

static int block_proxy_open(struct ext4_blockdev *bdev);
static int block_proxy_bread(struct ext4_blockdev *bdev, void *buf,
			     uint64_t blk_id, uint32_t blk_cnt);
static int block_proxy_bwrite(struct ext4_blockdev *bdev, const void *buf,
			      uint64_t blk_id, uint32_t blk_cnt);
static int block_proxy_close(struct ext4_blockdev *bdev);
static int block_proxy_lock(struct ext4_blockdev *bdev);
static int block_proxy_unlock(struct ext4_blockdev *bdev);

static char __first_hdd_name[32];

int ext4_blockdev_register(block *aux, char *name, int sec_size, int sec_cnt)
{
	uint8_t *_ph_bbuf = (uint8_t *)kmalloc(sec_size);
	struct ext4_blockdev *t = (struct ext4_blockdev *)kmalloc(sizeof(*t));
	struct ext4_blockdev_iface *block_iface =
		(struct ext4_blockdev_iface *)kmalloc(sizeof(*block_iface));
	memset((void *)block_iface, 0, sizeof(*block_iface));
	block_iface->open = block_proxy_open,
	block_iface->bread = block_proxy_bread,
	block_iface->bwrite = block_proxy_bwrite,
	block_iface->close = block_proxy_close,
	block_iface->lock = block_proxy_lock,
	block_iface->unlock = block_proxy_unlock,
	block_iface->ph_bsize = sec_size, block_iface->ph_bcnt = sec_cnt,
	block_iface->ph_bbuf = _ph_bbuf, memset((void *)t, 0, sizeof(*t));
	t->bdif = block_iface;
	t->part_offset = 0;
	t->part_size = (sec_size) * (sec_cnt);
	t->aux = aux;
	if (!__first_hdd_name[0])
		strcpy(__first_hdd_name, name);

	return ext4_device_register(t, NULL, name);
}

static int block_proxy_open(struct ext4_blockdev *bdev)
{
	return 0;
}

static int block_proxy_bread(struct ext4_blockdev *bdev, void *buf,
			     uint64_t blk_id, uint32_t blk_cnt)
{
	block *b = bdev->aux;
	char *tmp = (char *)buf;
	int i = 0;
	for (i = 0; i < blk_cnt; i++) {
		b->read(b->aux, blk_id + i, tmp + i * BLOCK_SECTOR_SIZE,
			BLOCK_SECTOR_SIZE);
	}

	return 0;
}

static int block_proxy_bwrite(struct ext4_blockdev *bdev, const void *buf,
			      uint64_t blk_id, uint32_t blk_cnt)
{
	block *b = bdev->aux;
	char *tmp = (char *)buf;
	int i = 0;
	for (i = 0; i < blk_cnt; i++) {
		b->write(b->aux, blk_id + i, tmp + i * BLOCK_SECTOR_SIZE,
			 BLOCK_SECTOR_SIZE);
	}

	return 0;
}

static int block_proxy_close(struct ext4_blockdev *bdev)
{
	kfree(bdev->bdif->ph_bbuf);
	kfree(bdev->bdif);
	kfree(bdev);
	return 0;
}

static int block_proxy_lock(struct ext4_blockdev *bdev)
{
	return 0;
}

static int block_proxy_unlock(struct ext4_blockdev *bdev)
{
	return 0;
}

static file *ext4_sb_open(super_block *sb, const char *path, int flag);

static super_operations ext4_sops = {
	.open = ext4_sb_open,
};

void fs_mount_root()
{
	task_struct *cur = CURRENT_TASK();
	cur->root = sget(&ext4_sops);
	ext4_mount(__first_hdd_name, "/", 0);
	ext4_cache_write_back("/", true);
}

#define UNIMPL() klog("unimplemented: %s\n", __func__)

static int fs_find_empty_fd(file_descriptor *fds)
{
	int i;
	for (i = 0; i < MAX_FD; i++) {
		if (fds[i].used == 0)
			return i;
	}
	return -1;
}

int fs_read(int fd, unsigned offset, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	ssize_t n;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_op || !fp->f_op->read)
		return -1;

	if (offset != (unsigned)-1)
		fp->f_pos = offset;

	n = fp->f_op->read(fp, buf, len, &fp->f_pos);
	return n < 0 ? -1 : (int)n;
}

int fs_write(int fd, unsigned offset, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	ssize_t n;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_op || !fp->f_op->write)
		return -1;

	if (offset != (unsigned)-1)
		fp->f_pos = offset;

	n = fp->f_op->write(fp, buf, len, &fp->f_pos);
	return n < 0 ? -1 : (int)n;
}

static int fs_resolve_symlink_path(const char *linkpath, char *linkcontent,
				   size_t name_len)
{
	char *r;
	char *saved = linkcontent + MAX_PATH - name_len - 1;
	if (*linkcontent == '/')
		return 0;

	saved[name_len] = '\0';
	strcpy(saved, linkcontent);
	strcpy(linkcontent, linkpath);
	r = strrchr(linkcontent, '/');
	*r = '\0';
	strcat(linkcontent, "/");
	strcat(linkcontent, saved);
	return 0;
}

static file *ext4_sb_open(super_block *sb, const char *path, int flag)
{
	ext4_file *f = NULL;
	ext4_dir *dir = NULL;
	char *linkcontent = NULL;
	size_t name_len;
	int ret;
	file *fp = NULL;
	struct stat s;

	if (path[strlen(path) - 1] == '/') {
		dir = calloc(1, sizeof(*dir));
		ret = ext4_dir_open(dir, path);
		if (ret != EOK)
			goto fail;
		fp = fs_alloc_filep_dir(dir);
		fp->f_mode = S_IFDIR;
	} else {
		f = calloc(1, sizeof(*f));
		ret = ext4_fopen2(f, path, flag);
		if (ret != EOK)
			goto fail;

		ret = ext4_fstat(f, &s);
		if (ret != EOK)
			goto fail;

		if (S_ISLNK(s.st_mode)) {
			linkcontent = name_get();
			memset(linkcontent, 0, MAX_PATH);
		}

		while (S_ISLNK(s.st_mode)) {
			ret = ext4_fread(f, linkcontent, PAGE_SIZE, &name_len);
			if (ret != EOK)
				goto fail;
			ext4_fclose(f);
			linkcontent[name_len] = '\0';
			fs_resolve_symlink_path(path, linkcontent, name_len);
			ret = ext4_fopen2(f, linkcontent, flag);
			if (ret != EOK)
				goto fail;

			ret = ext4_fstat(f, &s);
			if (ret != EOK)
				goto fail;
		}

		if (S_ISDIR(s.st_mode)) {
			ret = ext4_fclose(f);
			if (ret != EOK)
				goto fail;

			free(f);
			f = NULL;
			dir = calloc(1, sizeof(*dir));
			ret = ext4_dir_open(dir, path);
			if (ret != EOK)
				goto fail;
			ext4_dir_entry_rewind(dir);
			fp = fs_alloc_filep_dir(dir);
		} else {
			fp = fs_alloc_filep_normal(f);
		}
		fp->f_mode = s.st_mode;
	}

	goto done;
fail:
	fp = NULL;
	if (linkcontent)
		name_put(linkcontent);
	if (f)
		free(f);
	if (dir)
		free(dir);
done:
	return fp;
}

file *fs_open_file(const char *path, int flag, char *mode, int follow_link)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;

	if (cur->root)
		fp = vfs_open(cur->root, path, flag);

	if (fp)
		fp->f_name = strdup(path);

	return fp;
}

int fs_open(const char *path, int flag, char *mode)
{
	task_struct *cur = CURRENT_TASK();
	int fd = -1;
	file *fp = NULL;
	int ret = -ENOENT;
	mutex_lock(&cur->fd_lock);

	fd = fs_find_empty_fd(cur->fds);
	if (fd < 0)
		goto done;

	fp = fs_open_file(path, flag, mode, 1);
	if (!fp)
		goto fail;

	cur->fds[fd].flag = flag;
	cur->fds[fd].fp = fp;
	cur->fds[fd].used = 1;

	goto done;
fail:
	fd = ret;
done:
	mutex_unlock(&cur->fd_lock);
	return fd;
}

int fs_close(int fd)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	memset(&cur->fds[fd], 0, sizeof(file_descriptor));
	mutex_unlock(&cur->fd_lock);

	if (fp == NULL)
		return -1;

	return fs_put_file(fp);
}

int fs_delete(const char *path)
{
	/* FIXME: before virtual device (/dev etc) only ext4 support */
	int ret = ext4_fremove(path);
	if (ret != EOK)
		return -1;
	return ret;
}

int fs_stat(const char *path, struct stat *s)
{
	/* FIXME: before virtual device (/dev etc) only ext4 support */
	ext4_file f;
	ext4_dir dir;
	int isdir = 0;
	int ret = -1;

	if (path[strlen(path) - 1] == '/') {
		ret = ext4_dir_open(&dir, path);
		isdir = 1;
	} else {
		ret = ext4_fopen(&f, path, "r");
	}

	if (ret != EOK)
		return (0 - ret);

	if (!isdir) {
		ret = ext4_fstat(&f, s);
		ext4_fclose(&f);
	} else {
		ret = ext4_fstat(&dir.f, s);
		ext4_dir_close(&dir);
	}

	if (ret != EOK)
		return -1;

	return ret;
}

int fs_fstat(int fd, struct stat *s)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	mutex_unlock(&cur->fd_lock);

	if (!fp || !fp->f_inode || !fp->f_inode->i_op ||
	    !fp->f_inode->i_op->getattr)
		return -1;

	ret = fp->f_inode->i_op->getattr(fp->f_inode, s);
	return ret == EOK ? 0 : -1;
}

int fs_pipe(int *pipefd)
{
	int ret = 0;
	task_struct *cur = CURRENT_TASK();
	int reader, writer;
	file *fp[2] = { 0 };
	mutex_lock(&cur->fd_lock);
	reader = fs_find_empty_fd(cur->fds);
	if (reader < 0 || reader >= MAX_FD) {
		ret = -1;
		goto done;
	}
	/* reserve slot for writer search */
	cur->fds[reader].used = 1;

	writer = fs_find_empty_fd(cur->fds);
	if (writer < 0 || writer >= MAX_FD) {
		cur->fds[reader].used = 0;
		ret = -1;
		goto done;
	}
	cur->fds[writer].used = 1;
	ret = fs_alloc_filep_pipe(fp);
	if (ret != EOK) {
		cur->fds[reader].used = 0;
		cur->fds[writer].used = 0;
		ret = -1;
		goto done;
	}

	cur->fds[reader].fp = fp[0];
	cur->fds[writer].fp = fp[1];
	cur->fds[reader].flag = O_RDONLY;
	cur->fds[writer].flag = O_WRONLY;
	pipefd[0] = reader;
	pipefd[1] = writer;
	ret = 0;
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_dup(int fd)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int newfd;
	int ret;
	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;
	if (cur->fds[fd].used == 0)
		return -ENOENT;
	mutex_lock(&cur->fd_lock);
	newfd = fs_find_empty_fd(cur->fds);
	if (newfd < 0 || newfd >= MAX_FD) {
		ret = -1;
		goto done;
	}
	cur->fds[newfd] = cur->fds[fd];
	fp = cur->fds[fd].fp;
	fs_get_file(fp);
	cur->fds[newfd].flag &= ~O_CLOEXEC;
	ret = newfd;
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_dup2(int fd, int newfd)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret;
	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;

	if (newfd < 0 || newfd >= MAX_FD)
		return -EACCES;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	if (cur->fds[newfd].used)
		fs_put_file(cur->fds[newfd].fp);
	fp = cur->fds[fd].fp;
	fs_get_file(fp);
	cur->fds[newfd] = cur->fds[fd];
	cur->fds[newfd].flag &= ~O_CLOEXEC;
	ret = newfd;
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_put_file(file *f)
{
	if (__sync_add_and_fetch(&f->f_count, -1) == 0) {
		if (f->f_op && f->f_op->release)
			f->f_op->release(f->f_inode, f);
		if (f->f_name)
			free(f->f_name);
		free(f);
	}
	return 0;
}

int fs_llseek(int fd, unsigned offset_high, unsigned offset_low,
	      uint64_t *result, unsigned whence)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	loff_t offset = (loff_t)offset_high << 32 | (loff_t)offset_low;
	loff_t pos = -1;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_op || !fp->f_op->llseek)
		goto done;
	pos = fp->f_op->llseek(fp, offset, whence);
	if (pos >= 0) {
		fp->f_pos = pos;
		if (result)
			*result = (uint64_t)pos;
	}
done:
	mutex_unlock(&cur->fd_lock);
	return pos < 0 ? (int)pos : 0;
}

int fs_seek(int fd, unsigned offset, unsigned whence)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	loff_t pos = -EACCES;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_op || !fp->f_op->llseek)
		goto done;

	pos = fp->f_op->llseek(fp, offset, whence);
	if (pos >= 0)
		fp->f_pos = pos;
done:
	mutex_unlock(&cur->fd_lock);
	return pos < 0 ? (int)pos : 0;
}

int fs_select(int fd, unsigned type)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret = -EACCES;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_op || !fp->f_op->poll)
		goto done;

	ret = fp->f_op->poll(fp, type);
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_ioctl(int fd, unsigned cmd, void *buf)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret = -EACCES;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_op || !fp->f_op->ioctl)
		goto done;

	ret = fp->f_op->ioctl(fp, cmd, buf);
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_chmod(const char *pathname, uint32_t mode)
{
	int ret;
	ret = ext4_chmod(pathname, mode);
	return (0 - ret);
}

int fs_fchmod(int fd, uint32_t mode)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret = -EACCES;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	mutex_unlock(&cur->fd_lock);

	if (!fp || !fp->f_inode || !fp->f_inode->i_op ||
	    !fp->f_inode->i_op->setattr)
		return -EACCES;

	ret = fp->f_inode->i_op->setattr(fp->f_inode, mode);
	return (0 - ret);
}

int resolve_path(const char *old, char *new)
{
	char *r;
	int len = strlen(old);
	if (!old || !*old)
		return -1;

	if (!strcmp(old, ".")) {
		sys_getcwd(new, MAX_PATH);
		return 0;
	}

	if (!strcmp(old, "..")) {
		sys_getcwd(new, MAX_PATH);
		r = strrchr(new, '/');
		*r = '\0';
		if (*new == '\0') {
			*new ++ = '/';
			*new ++ = '\0';
		}
		return 0;
	}

	if (old[0] == '/') {
		strcpy(new, old);
		if (len >= 2 && new[len - 1] == '.' && new[len - 2] == '.') {
			r = strrchr(new, '/');
			if (!r)
				return -1;
			r = strrchr(r - 1, '/');
			if (!r)
				return -1;
			r++;
			*r = '\0';
		} else if (len >= 1 && new[len - 1] == '.') {
			new[len - 1] = '\0';
		}
		return 0;
	} else if (len > 1 && old[0] == '.' && old[1] == '/') {
		old += 2;
	}

	sys_getcwd(new, MAX_PATH);
	strcat(new, old);
	return 0;
}
