#include <mm/mm.h>
#include <lib/klib.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <hw/hdd.h>
#include <stddef.h>
#include <macro.h>
#include <config.h>
#include <ext4.h>

unsigned fs_read_size = 0;
unsigned fs_write_size = 0;

/* =========================================================================
 * ext4 file / directory operations
 * ====================================================================== */

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

static int ext4_file_chown(inode *node, uint32_t uid, uint32_t gid)
{
	ext4_file *f = node->i_private;
	return ext4_fchown(f, uid, gid);
}

static const inode_operations ext4_file_iops = {
	.getattr = ext4_file_getattr,
	.setattr = ext4_file_setattr,
	.chown = ext4_file_chown,
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
	const ext4_direntry *entry = NULL;
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
		len = ROUND_UP(NAME_OFFSET() +
			       strlen((const char *)entry->name) + 1);
		if (count < len) {
			if (prev)
				prev->d_off = retcount;
			break;
		}
		memset(dirp, 0, len);
		dirp->d_ino = entry->inode;
		strncpy(dirp->d_name, (const char *)entry->name,
			entry->name_length);
		dirp->d_reclen =
			ROUND_UP(NAME_OFFSET() + strlen(dirp->d_name) + 1);
		cur_pos += dirp->d_reclen;
		dirp->d_off = cur_pos;
		retcount += dirp->d_reclen;
		count -= dirp->d_reclen;
		prev = dirp;
		dirp = (struct linux_dirent *)((char *)dirp + dirp->d_reclen);
	}
	*pos += retcount;
	return (ssize_t)retcount;
}

static loff_t ext4_dir_llseek(file *fp, loff_t offset, int whence)
{
	ext4_dir *dir = fp->f_inode->i_private;
	const ext4_direntry *entry = NULL;
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
		len = ROUND_UP(NAME_OFFSET() +
			       strlen((const char *)entry->name) + 1);
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

static int ext4_dir_setattr(inode *node, uint32_t mode)
{
	ext4_dir *dir = node->i_private;
	return ext4_fchmod(&dir->f, mode);
}

static int ext4_dir_chown(inode *node, uint32_t uid, uint32_t gid)
{
	ext4_dir *dir = node->i_private;
	return ext4_fchown(&dir->f, uid, gid);
}

static const inode_operations ext4_dir_iops = {
	.getattr = ext4_dir_getattr,
	.setattr = ext4_dir_setattr,
	.chown = ext4_dir_chown,
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
	inode *node = zalloc(sizeof(*node));
	node->i_op = &ext4_file_iops;
	node->i_private = content;

	file *fp = zalloc(sizeof(*fp));
	fp->f_fop = &ext4_file_fops;
	fp->f_inode = node;
	fp->f_count = 1;
	return fp;
}

static file *ext4_alloc_dir(void *content)
{
	inode *node = zalloc(sizeof(*node));
	node->i_op = &ext4_dir_iops;
	node->i_private = content;

	file *fp = zalloc(sizeof(*fp));
	fp->f_fop = &ext4_dir_fops;
	fp->f_inode = node;
	fp->f_count = 1;
	return fp;
}

/* =========================================================================
 * ext4_path_open — symlink-aware open on an absolute lwext4 path.
 *
 * Shared by the root super_block and all secondary ext4 mounts.
 * Handles three cases:
 *   1. Trailing '/'          — open as directory.
 *   2. Regular file/symlink  — follow symlinks, then open.
 *   3. Symlink → directory   — reopen final target as directory.
 * ====================================================================== */

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

static file *ext4_path_open(const char *path, int flag)
{
	ext4_file *f = NULL;
	ext4_dir *dir = NULL;
	char *resolved = NULL;
	const char *cur_path = path;
	size_t link_len;
	int ret;
	int depth = 0;
	file *fp = NULL;
	struct stat s;

	/* ---- Case 1: caller-supplied trailing '/' means "open as dir" ---- */
	if (path[strlen(path) - 1] == '/') {
		dir = zalloc(sizeof(*dir));
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
	f = zalloc(sizeof(*f));
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

		if (fs_resolve_symlink_path(cur_path, resolved, link_len) != 0)
			goto fail;

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

		dir = zalloc(sizeof(*dir));
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

/* =========================================================================
 * Unified ext4 super_block
 *
 * Both the root mount ("/") and any secondary mount share the same sops.
 * Each super_block carries an ext4_mount_info in s_fs_info that records its
 * lwext4 mount point (always ending with '/').  Path construction in
 * ext4_open is therefore identical for all mounts:
 *
 *   root:      mp="/"      path="/etc/hosts" → "/etc/hosts"
 *   secondary: mp="/mnt/"  path="/etc/hosts" → "/mnt/etc/hosts"
 * ====================================================================== */

typedef struct {
	char mp[PAGE_SIZE]; /* lwext4 mount point with trailing '/', e.g. "/mnt/" */
} ext4_mount_info;

static file *ext4_open(super_block *sb, const char *path, int flag)
{
	ext4_mount_info *mi = sb->s_fs_info;
	char *full = vm_alloc(1);

	/* mp ends with '/'; path starts with '/' — skip path's leading '/' */
	sprintf(full, "%s%s", mi->mp, path[0] == '/' ? path + 1 : path);
	file *ret = ext4_path_open(full, flag);
	vm_free(full, 1);
	return ret;
}

static file *ext4_open_root(super_block *sb, int flag)
{
	ext4_mount_info *mi = sb->s_fs_info;

	return ext4_path_open(mi->mp, O_RDONLY);
}

static void ext4_release(super_block *sb)
{
	ext4_mount_info *mi = sb->s_fs_info;

	ext4_cache_write_back(mi->mp, false);
	ext4_umount(mi->mp);
	free(mi);
	kfree(sb);
}

/* Build the full lwext4 path for an operation on super_block sb. */
static void ext4_full_path(super_block *sb, const char *path, char *full)
{
	ext4_mount_info *mi = sb->s_fs_info;
	/* mi->mp ends with '/'; path starts with '/' — skip leading '/' */
	sprintf(full, "%s%s", mi->mp, path[0] == '/' ? path + 1 : path);
}

static int ext4_mkdir(super_block *sb, const char *path, unsigned mode)
{
	char *full = name_get();
	int ret;
	ext4_full_path(sb, path, full);
	ret = ext4_dir_mk(full);
	name_put(full);
	return ret ? -ret : 0;
}

static int ext4_rmdir(super_block *sb, const char *path)
{
	char *full = name_get();
	int ret;
	ext4_full_path(sb, path, full);
	ret = ext4_dir_rm(full);
	name_put(full);
	return ret ? -ret : 0;
}

static int ext4_unlink(super_block *sb, const char *path)
{
	char *full = name_get();
	int ret;
	ext4_full_path(sb, path, full);
	ret = ext4_fremove(full);
	name_put(full);
	return ret ? -ret : 0;
}

static int ext4_link(super_block *sb, const char *oldpath, const char *newpath)
{
	char *full1 = name_get();
	char *full2 = name_get();
	int ret;
	ext4_full_path(sb, oldpath, full1);
	ext4_full_path(sb, newpath, full2);
	ret = ext4_flink(full1, full2);
	name_put(full1);
	name_put(full2);
	return ret ? -ret : 0;
}

static int ext4_symlink_op(super_block *sb, const char *target,
			   const char *linkpath)
{
	char *full = name_get();
	int ret;
	ext4_full_path(sb, linkpath, full);
	ret = ext4_fsymlink(target, full);
	name_put(full);
	return ret ? -ret : 0;
}

static int ext4_rename(super_block *sb, const char *oldpath,
		       const char *newpath)
{
	char *full1 = name_get();
	char *full2 = name_get();
	int ret;
	ext4_full_path(sb, oldpath, full1);
	ext4_full_path(sb, newpath, full2);
	ret = ext4_frename(full1, full2);
	name_put(full1);
	name_put(full2);
	return ret ? -ret : 0;
}

static int ext4_readlink_op(super_block *sb, const char *path, char *buf,
			    size_t bufsiz, size_t *rcnt)
{
	char *full = name_get();
	int ret;
	ext4_full_path(sb, path, full);
	ret = ext4_readlink(full, buf, bufsiz, rcnt);
	name_put(full);
	return ret ? -ret : 0;
}

static super_operations ext4_sops = {
	.open_root = ext4_open_root,
	.open = ext4_open,
	.release = ext4_release,
	.mkdir = ext4_mkdir,
	.rmdir = ext4_rmdir,
	.unlink = ext4_unlink,
	.link = ext4_link,
	.symlink = ext4_symlink_op,
	.rename = ext4_rename,
	.readlink = ext4_readlink_op,
};

/* Allocate a super_block bound to the given lwext4 mount point. */
static super_block *ext4_new_sb(const char *mp)
{
	ext4_mount_info *mi = zalloc(sizeof(*mi));
	strncpy(mi->mp, mp, sizeof(mi->mp) - 1);

	super_block *sb = sget(&ext4_sops);
	sb->s_fs_info = mi;
	return sb;
}

/* Root factory: wraps the "/" lwext4 mount set up by fs_mount_root(). */
static super_block *ext4_get(void)
{
	return ext4_new_sb("/");
}

/*
 * ext4_get_sb — factory called by fs_do_mount() for "ext4" type mounts.
 *
 * Calls ext4_mount() on the requested device, then wraps the result in a
 * super_block using the unified ext4_sops.
 *
 * @dev:    block device path, e.g. "/dev/hda1" or bare "hda1"
 * @target: VFS mount point, e.g. "/mnt"  (must not be "/")
 * @flags:  MS_RDONLY etc.
 */
static super_block *ext4_get_sb(const char *dev, const char *target, int flags,
				void *data)
{
	const char *dev_name;
	char mp[MAX_PATH];
	size_t n;
	bool read_only;
	int i, ret;

	if (!dev || !target)
		return NULL;

	/* Strip "/dev/" prefix — lwext4 wants the raw partition name */
	dev_name = (strncmp(dev, "/dev/", 5) == 0) ? dev + 5 : dev;

	/* Verify the partition is known to the kernel */
	for (i = 0; i < hdd_partition_count; i++) {
		if (strcmp(hdd_partitions[i].name, dev_name) == 0)
			break;
	}
	if (i == hdd_partition_count)
		return NULL;

	/* lwext4 requires the mount point to end with '/' */
	strncpy(mp, target, sizeof(mp) - 2);
	mp[sizeof(mp) - 2] = '\0';
	n = strlen(mp);
	if (n > 0 && mp[n - 1] != '/') {
		mp[n] = '/';
		mp[n + 1] = '\0';
	}

	read_only = (flags & MS_RDONLY) != 0;
	ret = ext4_mount(dev_name, mp, read_only);
	if (ret != EOK)
		return NULL;

	if (!read_only)
		ext4_cache_write_back(mp, true);

	return ext4_new_sb(mp);
}

static fs_type ext4_fs_type = { .name = "ext4", .get_sb = ext4_get_sb };

/* =========================================================================
 * Boot-time root filesystem init
 * ====================================================================== */

static void fs_mount_root(void)
{
	task_struct *cur = CURRENT_TASK();

	/* Register the ext4 filesystem type so that sys_mount can use it */
	fs_register_type(&ext4_fs_type);

	cur->root = ext4_get();
	ext4_mount(hdd_partitions[0].name, "/", 0);
	ext4_cache_write_back("/", true);
}

KERNEL_INIT(3, fs_mount_root);
