#include <lock.h>
#include <macro.h>
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

unsigned fs_read_size = 0;
unsigned fs_write_size = 0;

static int ext4_file_release(inode *node, file *fp)
{
	ext4_file *f = node->i_private;
	ext4_fclose(f);
	free(f);
	free(node);
	return 0;
}

static ssize_t ext4_file_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	ext4_file *f = fp->f_inode->i_private;
	size_t rcnt = 0;
	/* Sync ext4 cursor with f_pos if they diverged (e.g. after pread) */
	if ((loff_t)ext4_ftell(f) != *pos)
		ext4_fseek(f, *pos, SEEK_SET);
	int ret = ext4_fread(f, buf, size, &rcnt);
	fs_read_size += rcnt;
	if (ret != EOK)
		return -1;
	*pos += rcnt;
	return (ssize_t)rcnt;
}

static ssize_t ext4_file_write(file *fp, const void *buf, size_t size,
			       loff_t *pos)
{
	ext4_file *f = fp->f_inode->i_private;
	size_t wcnt = 0;
	if ((loff_t)ext4_ftell(f) != *pos)
		ext4_fseek(f, *pos, SEEK_SET);
	int ret = ext4_fwrite(f, buf, size, &wcnt);
	fs_write_size += wcnt;
	if (ret != EOK)
		return -1;
	*pos += wcnt;
	return (ssize_t)wcnt;
}

static loff_t ext4_file_llseek(file *fp, loff_t offset, int whence)
{
	ext4_file *f = fp->f_inode->i_private;
	int ret;
	switch (whence) {
	case SEEK_SET:
		if ((uint64_t)offset > f->fsize) {
			ret = ext4_fenlarge(f, offset);
			if (ret != EOK)
				return (loff_t)(0 - ret);
		}
		break;
	case SEEK_CUR:
		if ((uint64_t)(offset + f->fpos) > f->fsize) {
			ret = ext4_fenlarge(f, offset + f->fpos);
			if (ret != EOK)
				return (loff_t)(0 - ret);
		}
		break;
	case SEEK_END:
		if ((uint64_t)offset > f->fsize)
			return -EINVAL;
		break;
	}
	ret = ext4_fseek(f, offset, whence);
	if (ret != EOK)
		return (loff_t)(0 - ret);
	return (loff_t)ext4_ftell(f);
}

static int ext4_file_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT)
		return -1;
	return 0;
}

static int ext4_file_flush(file *fp)
{
	/* Now we always flush */
	return 0;
}

static int ext4_file_getattr(inode *node, struct stat *s)
{
	ext4_file *f = node->i_private;
	return ext4_fstat(f, s);
}

static int ext4_file_setattr(inode *node, uint32_t mode)
{
	ext4_file *f = node->i_private;
	return ext4_fchmod(f, mode);
}

static const inode_operations ext4_file_iops = {
	.getattr = ext4_file_getattr,
	.setattr = ext4_file_setattr,
};

static const file_operations ext4_file_fops = {
	.release = ext4_file_release,
	.read = ext4_file_read,
	.write = ext4_file_write,
	.llseek = ext4_file_llseek,
	.poll = ext4_file_poll,
};

static int ext4_dir_release(inode *node, file *fp)
{
	ext4_dir *dir = node->i_private;
	ext4_dir_close(dir);
	free(dir);
	free(node);
	return 0;
}

static ssize_t ext4_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	ext4_dir *dir = fp->f_inode->i_private;
	struct linux_dirent *dirp = buf;
	ext4_direntry *entry = NULL;
	struct linux_dirent *prev = NULL;
	int retcount = 0;
	int len;
	int cur_pos = 0;

	while (count > 0) {
		entry = ext4_dir_entry_next(dir);
		if (entry == NULL) {
			if (prev)
				prev->d_off = retcount;
			break;
		}
		len = ROUND_UP(NAME_OFFSET() + strlen(entry->name) + 1);
		if (count < len) {
			if (prev)
				prev->d_off = retcount;
			break;
		}
		memset(dirp, 0, len);
		dirp->d_ino = entry->inode;
		strncpy(dirp->d_name, entry->name, entry->name_length);
		dirp->d_reclen =
			ROUND_UP(NAME_OFFSET() + strlen(dirp->d_name) + 1);
		cur_pos += dirp->d_reclen;
		dirp->d_off = cur_pos;
		retcount += dirp->d_reclen;
		count -= dirp->d_reclen;
		prev = dirp;
		dirp = (char *)dirp + dirp->d_reclen;
	}
	*pos += retcount;
	return (ssize_t)retcount;
}

static loff_t ext4_dir_llseek(file *fp, loff_t offset, int whence)
{
	ext4_dir *dir = fp->f_inode->i_private;
	ext4_direntry *entry = NULL;
	int len;
	int cur_pos = 0;
	int count = (int)offset;

	if (whence != SEEK_SET)
		return -EACCES;

	if (offset < (loff_t)sizeof(struct linux_dirent))
		return 0;

	ext4_dir_entry_rewind(dir);
	while (count > 0) {
		entry = ext4_dir_entry_next(dir);
		if (entry == NULL)
			break;
		len = ROUND_UP(NAME_OFFSET() + strlen(entry->name) + 1);
		if (count < len)
			return (loff_t)(cur_pos + len);
		cur_pos += len;
		count -= len;
	}
	return (loff_t)cur_pos;
}

static int ext4_dir_getattr(inode *node, struct stat *s)
{
	ext4_dir *dir = node->i_private;
	return ext4_fstat(&dir->f, s);
}

static const inode_operations ext4_dir_iops = {
	.getattr = ext4_dir_getattr,
};

static const file_operations ext4_dir_fops = {
	.release = ext4_dir_release,
	.read = ext4_dir_read,
	.llseek = ext4_dir_llseek,
	.poll = ext4_file_poll,
	.flush = ext4_file_flush,
};

file *fs_alloc_filep_normal(void *content)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_op = &ext4_file_iops;
	node->i_fop = &ext4_file_fops;
	node->i_private = content;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_op = &ext4_file_fops;
	fp->f_count = 1;
	return fp;
}

file *fs_alloc_filep_dir(void *content)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_op = &ext4_dir_iops;
	node->i_fop = &ext4_dir_fops;
	node->i_private = content;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_op = &ext4_dir_fops;
	fp->f_count = 1;
	return fp;
}

static file *ext4_sb_open(super_block *sb, const char *path, int flag);

static super_operations ext4_sops = {
	.open = ext4_sb_open,
};

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

/*
 * fs_resolve_symlink_path - make a symlink target into an absolute path.
 *
 * @linkpath:    the path of the symlink itself (used to derive its directory)
 * @linkcontent: buffer (MAX_PATH) holding the raw symlink target; updated
 *               in-place to the resolved absolute path on return
 * @name_len:    length of the symlink target string in linkcontent
 *
 * If the target is already absolute (starts with '/') it is kept as-is.
 * For a relative target the directory part of linkpath is prepended.
 * Returns 0 on success, -1 if the result would exceed MAX_PATH.
 */
static int fs_resolve_symlink_path(const char *linkpath, char *linkcontent,
				   size_t name_len)
{
	const char *r;
	size_t base_len;

	/* Absolute target: nothing to do */
	if (*linkcontent == '/')
		return 0;

	/* Find the directory component of the path that contained the symlink */
	r = strrchr(linkpath, '/');
	if (!r)
		return -1;
	base_len = (size_t)(r - linkpath) + 1; /* include the trailing '/' */

	if (base_len + name_len >= MAX_PATH)
		return -1;

	/* Shift target right to make room for the base prefix, then prepend it.
	 * memmove handles the overlap that would occur when name_len is large. */
	memmove(linkcontent + base_len, linkcontent, name_len + 1);
	memcpy(linkcontent, linkpath, base_len);
	return 0;
}

#define MAX_SYMLINK_DEPTH 8

/*
 * ext4_sb_open - open a path on an ext4 superblock, following symlinks.
 *
 * Handles three cases:
 *   1. Path ending in '/' — open directly as a directory.
 *   2. Regular file — open with ext4_fopen2, follow symlinks if needed.
 *   3. Symlink that ultimately resolves to a directory — reopen as dir.
 *
 * Populates f_mode and all three inode fields (i_mode, i_ino, i_size)
 * from the stat of the final target.
 */
static file *ext4_sb_open(super_block *sb, const char *path, int flag)
{
	ext4_file *f = NULL;
	ext4_dir *dir = NULL;
	/* resolved: heap buffer for the current symlink-resolved path */
	char *resolved = NULL;
	/* cur_path: points to either the original path or resolved buffer */
	const char *cur_path = path;
	size_t link_len;
	int ret;
	int depth = 0;
	file *fp = NULL;
	struct stat s;

	/* ---- Case 1: caller-supplied trailing '/' means "open as dir" ---- */
	if (path[strlen(path) - 1] == '/') {
		dir = calloc(1, sizeof(*dir));
		ret = ext4_dir_open(dir, path);
		if (ret != EOK)
			goto fail;
		ret = ext4_fstat(&dir->f, &s);
		if (ret != EOK)
			goto fail;
		fp = fs_alloc_filep_dir(dir);
		fp->f_mode = s.st_mode;
		fp->f_inode->i_mode = s.st_mode;
		fp->f_inode->i_ino = s.st_ino;
		fp->f_inode->i_size = s.st_size;
		goto done;
	}

	/* ---- Case 2: regular open, with symlink following ---- */
	f = calloc(1, sizeof(*f));
	ret = ext4_fopen2(f, path, flag);
	if (ret != EOK)
		goto fail;

	ret = ext4_fstat(f, &s);
	if (ret != EOK)
		goto fail;

	/* Allocate a resolution buffer only when we actually encounter a symlink */
	if (S_ISLNK(s.st_mode)) {
		resolved = name_get();
		if (!resolved)
			goto fail;
	}

	while (S_ISLNK(s.st_mode)) {
		/* Guard against symlink loops */
		if (++depth > MAX_SYMLINK_DEPTH)
			goto fail;

		/* Read the raw symlink target into the resolution buffer */
		ret = ext4_fread(f, resolved, MAX_PATH - 1, &link_len);
		ext4_fclose(f);
		if (ret != EOK)
			goto fail;
		resolved[link_len] = '\0';

		/* For relative targets, resolve against the directory of cur_path.
		 * On the first iteration cur_path == path; on subsequent iterations
		 * it equals resolved (the previous loop's output), so chained
		 * relative symlinks are resolved correctly. */
		if (fs_resolve_symlink_path(cur_path, resolved, link_len) != 0)
			goto fail;

		/* resolved now holds the next path to open */
		cur_path = resolved;

		ret = ext4_fopen2(f, cur_path, flag);
		if (ret != EOK)
			goto fail;

		ret = ext4_fstat(f, &s);
		if (ret != EOK)
			goto fail;
	}

	/* ---- Case 3: final target is a directory ---- */
	if (S_ISDIR(s.st_mode)) {
		ext4_fclose(f);
		free(f);
		f = NULL;

		dir = calloc(1, sizeof(*dir));
		/* Use cur_path: it holds the resolved target, not the original symlink */
		ret = ext4_dir_open(dir, cur_path);
		if (ret != EOK)
			goto fail;
		ext4_dir_entry_rewind(dir);
		fp = fs_alloc_filep_dir(dir);
	} else {
		fp = fs_alloc_filep_normal(f);
	}

	fp->f_mode = s.st_mode;
	fp->f_inode->i_mode = s.st_mode;
	fp->f_inode->i_ino = s.st_ino;
	fp->f_inode->i_size = s.st_size;
	goto done;

fail:
	fp = NULL;
	if (f)
		free(f);
	if (dir)
		free(dir);
done:
	if (resolved)
		name_put(resolved);
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
	return (int)pos;
}

int fs_seek(int fd, unsigned offset, unsigned whence)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	loff_t pos = -EACCES;
	int ret = 0;
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
	return (int)pos;
}

int fs_sync(int fd)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret = -ENOENT;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;

	if (!fp || !fp->f_inode || !fp->f_inode->i_fop)
		goto done;

	if (fp->f_inode->i_fop->flush)
		ret = fp->f_inode->i_fop->flush(fp);
	else
		ret = 0;
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
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

/*
 * resolve_path - turn a user-supplied path into an absolute path.
 *
 * Handles:
 *   - NULL / empty input                     → error
 *   - Exact "."  / ".."                      → cwd / parent of cwd
 *   - Absolute paths (start with '/')        → copy, then strip trailing
 *                                              "/.." or "/." components
 *   - Relative paths (including "./" prefix) → prepend cwd
 *
 * The result is written into `new` (caller must supply at least MAX_PATH
 * bytes).  Returns 0 on success, -1 on error.
 */
int resolve_path(const char *old, char *new)
{
	char *r;
	int len;

	/* Null check must precede any use of old */
	if (!old || !*old)
		return -1;

	len = strlen(old);

	/* "." — current directory */
	if (!strcmp(old, ".")) {
		sys_getcwd(new, MAX_PATH);
		return 0;
	}

	/* ".." — parent of current directory */
	if (!strcmp(old, "..")) {
		sys_getcwd(new, MAX_PATH);
		r = strrchr(new, '/');
		if (r == new) {
			/* Already at root: stay at "/" */
			new[0] = '/';
			new[1] = '\0';
		} else {
			*r = '\0';
		}
		return 0;
	}

	if (old[0] == '/') {
		/* Absolute path: copy verbatim, then normalise trailing component */
		strcpy(new, old);

		if (len >= 3 && new[len-1] == '.' && new[len-2] == '.' &&
		    new[len-3] == '/') {
			/* Trailing "/.." — strip it, then strip the last dir component.
			 * Two-step: first remove the "/.." suffix to get the parent
			 * dir string, then find the slash before that dir name. */
			new[len-3] = '\0';           /* e.g. "/foo/bar"  */
			r = strrchr(new, '/');
			if (!r) {
				/* Path was something like "/bar/.." — result is root */
				new[0] = '/';
				new[1] = '\0';
			} else if (r == new) {
				/* Parent is root: "/foo/.." → "/" */
				new[1] = '\0';
			} else {
				/* General case: "/foo/bar/.." → "/foo/" */
				*(r + 1) = '\0';
			}
		} else if (len >= 2 && new[len-1] == '.' && new[len-2] == '/') {
			/* Trailing "/." — strip the dot, keep the slash */
			new[len-1] = '\0';
		}
		return 0;
	}

	/* Relative path: strip leading "./" if present */
	if (len > 1 && old[0] == '.' && old[1] == '/')
		old += 2;

	/* Prepend cwd, ensuring a '/' separator between cwd and old */
	sys_getcwd(new, MAX_PATH);
	len = strlen(new);
	if (len > 0 && new[len - 1] != '/')
		new[len++] = '/';
	strcat(new + len, old);
	return 0;
}

static void fs_mount_root()
{
	task_struct *cur = CURRENT_TASK();
	cur->root = sget(&ext4_sops);
	ext4_mount(__first_hdd_name, "/", 0);
	ext4_cache_write_back("/", true);
}

KERNEL_INIT(3, fs_mount_root);
