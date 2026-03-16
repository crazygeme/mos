#include <lib/klib.h>
#include <fs/fs.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <hw/hdd.h>
#include <stddef.h>
#include <macro.h>
#include <ext4.h>

unsigned fs_read_size = 0;
unsigned fs_write_size = 0;

static int ext4_file_release(file *fp)
{
	ext4_file *f = fp->f_inode->i_private;
	ext4_fclose(f);
	free(f);
	free(fp->f_inode);
	free(fp);
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

static int ext4_dir_release(file *fp)
{
	ext4_dir *dir = fp->f_inode->i_private;
	ext4_dir_close(dir);
	free(dir);
	free(fp->f_inode);
	free(fp);
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

static file *ext4_alloc_file(void *content)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_op = &ext4_file_iops;
	node->i_private = content;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_fop = &ext4_file_fops;
	fp->f_inode = node;
	fp->f_count = 1;
	return fp;
}

static file *ext4_alloc_dir(void *content)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_op = &ext4_dir_iops;
	node->i_private = content;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_fop = &ext4_dir_fops;
	fp->f_inode = node;
	fp->f_count = 1;
	return fp;
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
		fp = ext4_alloc_dir(dir);
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
		fp = ext4_alloc_dir(dir);
	} else {
		fp = ext4_alloc_file(f);
	}

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

static super_operations ext4_sops = {
	.open = ext4_sb_open,
};

super_block *ext4_get()
{
	return sget(&ext4_sops);
}

static void fs_mount_root()
{
	task_struct *cur = CURRENT_TASK();
	cur->root = ext4_get();
	ext4_mount(hdd_partitions[0].name, "/", 0);
	ext4_cache_write_back("/", true);
}

KERNEL_INIT(3, fs_mount_root);