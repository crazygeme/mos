#include <mm/mm.h>
#include <mm/mmap.h>
#include <dev/blockdev.h>
#include <fs/cache.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <hw/hdd.h>
#include <dev/loopdev.h>
#include <hw/time.h>
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

	if ((fp->f_state & FS_FILE_UNLINK_ON_CLOSE) && fp->f_name)
		ext4_fremove(fp->f_name);
	ext4_fclose(f);
	free(f);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static ssize_t ext4_file_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	ext4_file *f = fp->f_inode->i_private;
	ssize_t rcnt = fs_page_cache_read(fp, buf, size, pos);

	if (rcnt < 0)
		return rcnt;

	/*
	 * Keep the underlying ext4 cursor coherent with f_pos even though the
	 * data path now goes through the shared fs page cache. Callers such as
	 * llseek(SEEK_CUR) still consult ext4_ftell().
	 */
	if (*pos <= (loff_t)f->fsize && (loff_t)ext4_ftell(f) != *pos)
		ext4_fseek(f, *pos, SEEK_SET);

	fs_read_size += (unsigned)rcnt;
	if (fp->f_name)
		ext4_file_set_atime(fp->f_name, (uint32_t)time_now_sec());
	return rcnt;
}

static ssize_t ext4_file_write(file *fp, const void *buf, size_t size,
			       loff_t *pos)
{
	ext4_file *f = fp->f_inode->i_private;
	size_t wcnt = 0;
	loff_t write_pos = *pos;
	int ret;

	if (fp->f_inode)
		fs_page_cache_invalidate(fp);
	if (fp->f_flag & O_APPEND)
		write_pos = (loff_t)f->fsize;
	if ((uint64_t)write_pos > f->fsize) {
		ret = ext4_fenlarge(f, (uint64_t)write_pos);
		if (ret != EOK)
			return -1;
	}
	if ((loff_t)ext4_ftell(f) != write_pos)
		ext4_fseek(f, write_pos, SEEK_SET);
	ret = ext4_fwrite(f, buf, size, &wcnt);
	fs_write_size += wcnt;
	if (ret != EOK)
		return -1;
	*pos = write_pos + (loff_t)wcnt;
	if (fp->f_inode)
		fp->f_inode->i_size = f->fsize;
	if (fp->f_name) {
		uint32_t t = (uint32_t)time_now_sec();
		ext4_file_set_mtime(fp->f_name, t);
	}
	return (ssize_t)wcnt;
}

static loff_t ext4_file_llseek(file *fp, loff_t offset, int whence)
{
	ext4_file *f = fp->f_inode->i_private;
	loff_t new_pos;
	loff_t cur = fp ? fp->f_pos : (loff_t)ext4_ftell(f);

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = cur + offset;
		break;
	case SEEK_END:
		new_pos = (loff_t)f->fsize + offset;
		break;
	default:
		return -EINVAL;
	}

	if (new_pos < 0)
		return -EINVAL;

	/*
	 * Linux lseek only updates the VFS file position. Seeking past EOF is
	 * legal, but lwext4's ext4_fseek() rejects it, so the backing cursor is
	 * synchronized later by read/write when I/O actually happens.
	 */
	if (fp)
		fp->f_pos = new_pos;

	return new_pos;
}

static unsigned ext4_file_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return events & (FS_POLL_READ | FS_POLL_WRITE);
}

static int ext4_file_flush(file *fp)
{
	super_block *sb = NULL;
	const char *path = NULL;
	int ret;

	if (!fp)
		return -EINVAL;

	if (fp->f_inode && fp->f_inode->i_pgcache_tag)
		sb = fp->f_inode->i_pgcache_tag;

	if (fp->f_name)
		path = fp->f_name;
	else if (sb)
		path = sb->s_mountpoint;

	if (!path)
		return 0;

	if (CURRENT_TASK() && current->user)
		vm_flush_file_dirty(current->user->vm, fp);

	/*
	 * rw lwext4 mounts keep delayed write-back enabled. Toggle it off once
	 * to force pending filesystem buffers out, then restore the mount's
	 * previous policy so subsequent writes keep the same behavior.
	 */
	ret = ext4_cache_write_back(path, false);
	if (ret != EOK)
		return -EIO;

	if (sb && !(sb->s_flags & MS_RDONLY)) {
		ret = ext4_cache_write_back(path, true);
		if (ret != EOK)
			return -EIO;
	}

	hdd_flush();
	return 0;
}

static int ext4_file_getattr(file *fp, struct stat *s)
{
	ext4_file *f = fp->f_inode->i_private;
	return ext4_fstat(f, s);
}

static int ext4_file_setattr(file *fp, uint32_t mode)
{
	ext4_file *f = fp->f_inode->i_private;
	return ext4_fchmod(f, mode);
}

static int ext4_file_chown(file *fp, uint32_t uid, uint32_t gid)
{
	ext4_file *f = fp->f_inode->i_private;
	return ext4_fchown(f, uid, gid);
}

static int ext4_file_read_page(file *fp, unsigned offset, void *buf)
{
	ext4_file *ff = fp->f_inode->i_private;
	loff_t saved_pos = (loff_t)ext4_ftell(ff);
	size_t rcnt = 0;
	int ret;

	if (ext4_fseek(ff, offset, SEEK_SET) != EOK)
		return -EIO;
	ret = ext4_fread(ff, buf, PAGE_SIZE, &rcnt);
	if (rcnt < PAGE_SIZE)
		memset((char *)buf + rcnt, 0, PAGE_SIZE - rcnt);
	ext4_fseek(ff, saved_pos, SEEK_SET);
	if (ret != EOK)
		return -EIO;
	return 0;
}

static int ext4_file_write_page(file *fp, unsigned offset, const void *buf)
{
	ext4_file *ff = fp->f_inode->i_private;
	loff_t saved_pos = (loff_t)ext4_ftell(ff);
	size_t wcnt = 0;
	int ret;

	fs_page_cache_invalidate(fp);
	if (ext4_fseek(ff, offset, SEEK_SET) != EOK)
		return -EIO;
	ret = ext4_fwrite(ff, buf, PAGE_SIZE, &wcnt);
	ext4_fseek(ff, saved_pos, SEEK_SET);
	if (ret != EOK)
		return -EIO;
	return 0;
}

static int ext4_file_ftruncate(file *fp, loff_t size)
{
	ext4_file *f = fp->f_inode->i_private;
	int ret;

	if (size < 0)
		return -EINVAL;

	fs_page_cache_invalidate(fp);
	if ((uint64_t)size > f->fsize)
		ret = ext4_fenlarge(f, (uint64_t)size);
	else
		ret = ext4_ftruncate(f, (uint64_t)size);
	if (ret != EOK)
		return -ret;

	if (fp->f_inode)
		fp->f_inode->i_size = f->fsize;
	if (fp->f_pos > size)
		fp->f_pos = size;
	if (fp->f_name) {
		uint32_t t = (uint32_t)time_now_sec();
		ext4_file_set_mtime(fp->f_name, t);
		ext4_file_set_ctime(fp->f_name, t);
	}
	return 0;
}

static const file_operations ext4_file_fops = {
	.release = ext4_file_release,
	.getattr = ext4_file_getattr,
	.setattr = ext4_file_setattr,
	.chown = ext4_file_chown,
	.read = ext4_file_read,
	.write = ext4_file_write,
	.llseek = ext4_file_llseek,
	.poll = ext4_file_poll,
	.read_page = ext4_file_read_page,
	.write_page = ext4_file_write_page,
	.ftruncate = ext4_file_ftruncate,
	.flush = ext4_file_flush,
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
		unsigned namelen;

		entry = ext4_dir_entry_next(dir);
		if (entry == NULL) {
			if (prev)
				prev->d_off = retcount;
			break;
		}
		if (entry->inode == 0 || entry->name_length == 0 ||
		    entry->inode_type == EXT4_DIRENTRY_DIR_CSUM)
			continue;
		namelen = entry->name_length;
		len = ROUND_UP(NAME_OFFSET() + namelen + 1);
		if (count < len) {
			if (prev)
				prev->d_off = retcount;
			break;
		}
		memset(dirp, 0, len);
		dirp->d_ino = entry->inode;
		memcpy(dirp->d_name, entry->name, namelen);
		dirp->d_name[namelen] = '\0';
		dirp->d_reclen = (unsigned short)len;
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
		unsigned namelen;

		entry = ext4_dir_entry_next(dir);
		if (entry == NULL)
			break;
		if (entry->inode == 0 || entry->name_length == 0 ||
		    entry->inode_type == EXT4_DIRENTRY_DIR_CSUM)
			continue;
		namelen = entry->name_length;
		len = ROUND_UP(NAME_OFFSET() + namelen + 1);
		if (count < len)
			return (loff_t)(cur_pos + len);
		cur_pos += len;
		count -= len;
	}
	return (loff_t)cur_pos;
}

static int ext4_dir_getattr(file *fp, struct stat *s)
{
	ext4_dir *dir = fp->f_inode->i_private;
	return ext4_fstat(&dir->f, s);
}

static int ext4_dir_setattr(file *fp, uint32_t mode)
{
	ext4_dir *dir = fp->f_inode->i_private;
	return ext4_fchmod(&dir->f, mode);
}

static int ext4_dir_chown(file *fp, uint32_t uid, uint32_t gid)
{
	ext4_dir *dir = fp->f_inode->i_private;
	return ext4_fchown(&dir->f, uid, gid);
}

static const file_operations ext4_dir_fops = {
	.release = ext4_dir_release,
	.getattr = ext4_dir_getattr,
	.setattr = ext4_dir_setattr,
	.chown = ext4_dir_chown,
	.read = ext4_dir_read,
	.llseek = ext4_dir_llseek,
	.poll = ext4_file_poll,
	.flush = ext4_file_flush,
};

static file *ext4_alloc_file(void *content)
{
	inode *node = zalloc(sizeof(*node));
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

/*
 * ext4_resolve_prefix - resolve symlinks in all intermediate (non-final)
 * path components so that lwext4 can traverse them.
 *
 * lwext4's ext4_generic_open2 rejects any non-final component that is not
 * a directory (error: "expected directory").  Symlinks in intermediate
 * positions therefore cause ENOENT.  This helper walks left-to-right,
 * opening each component as the *goal* (which works for symlinks via the
 * EXT4_DE_SYMLINK filetype), and substitutes any symlink with its target
 * before continuing.  Because each step only extends an already-resolved
 * prefix, ext4_fopen2 can always traverse the accumulated path.
 *
 * The final component is left verbatim; the caller is responsible for
 * following it (or not) based on flags such as O_NOFOLLOW.
 *
 * @path: absolute lwext4 path, must start with '/'
 * @out:  output buffer, at least MAX_PATH bytes
 *
 * Returns 0 on success, -1 on error.  `out` is unchanged on error.
 */
static int ext4_resolve_prefix(const char *path, char *out)
{
	const char *last_slash, *p, *end;
	char *work, *tgt;
	size_t comp_len, work_len, link_len;
	ext4_file f;
	struct stat s;
	int depth = 0, ret;

	last_slash = strrchr(path, '/');
	/* Nothing intermediate to resolve when there is no directory prefix. */
	if (!last_slash || last_slash == path) {
		if (strlen(path) < MAX_PATH) {
			strcpy(out, path);
			return 0;
		}
		return -1;
	}

	work = name_get();
	work[0] = '\0';
	p = path + 1; /* skip leading '/' */

	while (p < last_slash) {
		/* Isolate the next component (up to but not past last_slash). */
		end = strchr(p, '/');
		if (!end || end >= last_slash)
			end = last_slash;
		comp_len = (size_t)(end - p);
		work_len = strlen(work);

		if (work_len + 1 + comp_len + 1 >= MAX_PATH)
			goto err;

		work[work_len] = '/';
		memcpy(work + work_len + 1, p, comp_len);
		work[work_len + 1 + comp_len] = '\0';

		/*
		 * Open `work` with the current component as the goal.
		 * All prior components in `work` are already-resolved real
		 * directories, so lwext4 can traverse them.  ext4_fopen2 also
		 * tries EXT4_DE_SYMLINK, so it succeeds even for symlinks.
		 */
		memset(&f, 0, sizeof(f));
		ret = ext4_fopen2(&f, work, O_RDONLY);
		if (ret != EOK)
			goto err;

		ret = ext4_fstat(&f, &s);
		if (ret != EOK) {
			ext4_fclose(&f);
			goto err;
		}

		if (S_ISLNK(s.st_mode)) {
			if (++depth > MAX_SYMLINK_DEPTH) {
				ext4_fclose(&f);
				goto err;
			}
			tgt = name_get();
			ret = ext4_fread(&f, tgt, MAX_PATH - 1, &link_len);
			ext4_fclose(&f);
			if (ret != EOK) {
				name_put(tgt);
				goto err;
			}
			tgt[link_len] = '\0';
			if (fs_resolve_symlink_path(work, tgt, link_len) != 0 ||
			    strlen(tgt) >= MAX_PATH) {
				name_put(tgt);
				goto err;
			}
			strcpy(work, tgt);
			name_put(tgt);
		} else {
			ext4_fclose(&f);
		}

		p = end + 1;
	}

	/* Append the final component verbatim (last_slash starts with '/'). */
	work_len = strlen(work);
	if (work_len + strlen(last_slash) >= MAX_PATH)
		goto err;
	strcpy(out, work);
	strcat(out, last_slash);
	name_put(work);
	return 0;
err:
	name_put(work);
	return -1;
}

static file *ext4_path_open(const char *path, int flag)
{
	ext4_file *f = NULL;
	ext4_dir *dir = NULL;
	unsigned uid = current->user->uid;
	unsigned gid = current->user->gid;
	char *pre_res = NULL; /* buffer for intermediate symlink resolution */
	char *resolved = NULL; /* buffer for final symlink following */
	const char *cur_path = path;
	size_t link_len;
	int ret, check;
	int depth = 0;
	file *fp = NULL;
	struct stat s;

	/*
	 * Resolve symlinks in all intermediate path components.
	 * lwext4 does not follow symlinks in non-final components; without
	 * this step, paths like "/var/mail/foo" (where "mail" is a symlink)
	 * return ENOENT from lwext4.
	 */
	pre_res = name_get();
	if (pre_res && ext4_resolve_prefix(path, pre_res) == 0)
		cur_path = pre_res;

	/* ---- Case 1: trailing '/' means "open as dir" ---- */
	if (cur_path[strlen(cur_path) - 1] == '/') {
		dir = zalloc(sizeof(*dir));
		ret = ext4_dir_open(dir, cur_path);
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

	/* ---- Case 2: regular open, with final symlink following ---- */
	f = zalloc(sizeof(*f));
	/*
	 * The pre-check (open without O_CREAT) is only needed when O_CREAT is
	 * set, to detect whether the file was just created so we can assign
	 * ownership.  Skipping it for non-O_CREAT opens halves the number of
	 * ext4_dir_find_entry calls on the hot path.
	 */
	if (flag & O_CREAT) {
		check = ext4_fopen2(f, cur_path, (flag & ~O_CREAT));
	} else {
		check = EOK;
	}
	ret = ext4_fopen2(f, cur_path, flag);
	if (check != EOK && ret == EOK) {
		ext4_fchown(f, uid, gid);
		ext4_file_set_ctime(cur_path, time_now_sec());
	}

	if (ret != EOK)
		goto fail;

	ret = ext4_fstat(f, &s);
	if (ret != EOK)
		goto fail;
	/* O_NOFOLLOW: return the symlink itself without following it. */
	if (S_ISLNK(s.st_mode) && (flag & O_NOFOLLOW)) {
		fp = ext4_alloc_file(f);
		fp->f_inode->i_mode = s.st_mode;
		fp->f_inode->i_ino = s.st_ino;
		fp->f_inode->i_size = s.st_size;
		fp->f_name = strdup(path); /* original (pre-resolution) path */
		goto done;
	}

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

		/*
		 * Read the symlink target into `resolved`.  Note: `resolved`
		 * and `pre_res` are distinct buffers, so cur_path (which may
		 * point into pre_res) remains valid for fs_resolve_symlink_path.
		 */
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
	fp->f_name = strdup(cur_path);
	goto done;

fail:
	fp = NULL;
	if (f)
		free(f);
	if (dir)
		free(dir);
done:
	if (pre_res)
		name_put(pre_res);
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
	char devname[64]; /* partition name, e.g. "hda1" — used for remount */
	char loop_name[16]; /* non-empty if mount auto-attached a loop device */
} ext4_mount_info;

static file *ext4_open(super_block *sb, const char *path, int flag)
{
	ext4_mount_info *mi = sb->s_fs_info;
	char *full = name_get();

	/* mp ends with '/'; path starts with '/' — skip path's leading '/' */
	sprintf(full, "%s%s", mi->mp, path[0] == '/' ? path + 1 : path);
	file *ret = ext4_path_open(full, flag);
	if (ret && ret->f_inode) {
		ret->f_inode->i_pgcache_tag = sb;
		if (flag & O_TRUNC)
			fs_page_cache_invalidate(ret);
	}
	name_put(full);
	return ret;
}

static file *ext4_open_root(super_block *sb, int flag)
{
	ext4_mount_info *mi = sb->s_fs_info;
	file *ret = ext4_path_open(mi->mp, O_RDONLY);

	if (ret && ret->f_inode)
		ret->f_inode->i_pgcache_tag = sb;
	return ret;
}

static void ext4_release(super_block *sb)
{
	ext4_mount_info *mi = sb->s_fs_info;

	ext4_cache_write_back(mi->mp, false);
	ext4_umount(mi->mp);
	if (mi->loop_name[0])
		loop_teardown(mi->loop_name);
	free(mi);
	kfree(sb);
}

static int fs_sync_super_one(const super_block *sb)
{
	ext4_mount_info *mi;
	int ret;

	if (!sb || !sb->s_fs_info)
		return 0;

	if (strcmp(sb->s_fstype, "ext4") != 0 &&
	    strcmp(sb->s_fstype, "ext3") != 0 &&
	    strcmp(sb->s_fstype, "vfat") != 0)
		return 0;

	mi = sb->s_fs_info;

	/*
	 * rw lwext4 mounts keep write-back mode enabled persistently.
	 * Drop it once to force dirty buffers out, then restore the mount's
	 * previous delayed-write policy.
	 */
	ret = ext4_cache_write_back(mi->mp, false);
	if (ret != EOK)
		return -EIO;

	if (!(sb->s_flags & MS_RDONLY)) {
		ret = ext4_cache_write_back(mi->mp, true);
		if (ret != EOK)
			return -EIO;
	}

	return 0;
}

int fs_sync_super(const super_block *sb)
{
	super_block *cur = (super_block *)sb;
	key_value_pair *kv;
	int ret;

	if (!sb)
		return 0;

	ret = fs_sync_super_one(sb);
	if (ret)
		return ret;

	mutex_lock(&cur->s_lock);
	for (kv = hash_first(cur->s_mounts); kv;
	     kv = hash_next(cur->s_mounts, kv)) {
		super_block *child = kv->val;

		mutex_unlock(&cur->s_lock);
		ret = fs_sync_super(child);
		if (ret)
			return ret;
		mutex_lock(&cur->s_lock);
	}
	mutex_unlock(&cur->s_lock);

	return 0;
}

/* Build the full lwext4 path for an operation on super_block sb. */
static void ext4_trim_trailing_slashes(char *path)
{
	int len = strlen(path);

	while (len > 1 && path[len - 1] == '/') {
		path[len - 1] = '\0';
		len--;
	}
}

static void ext4_full_path(super_block *sb, const char *path, char *full)
{
	ext4_mount_info *mi = sb->s_fs_info;
	/* mi->mp ends with '/'; path starts with '/' — skip leading '/' */
	sprintf(full, "%s%s", mi->mp, path[0] == '/' ? path + 1 : path);
	ext4_trim_trailing_slashes(full);
}

static int ext4_path_is_descendant(const char *parent, const char *path)
{
	size_t parent_len = strlen(parent);

	while (parent_len > 1 && parent[parent_len - 1] == '/')
		parent_len--;

	return strncmp(parent, path, parent_len) == 0 &&
	       path[parent_len] == '/';
}

static int ext4_dir_check_empty(const char *full)
{
	ext4_dir dir;
	const ext4_direntry *entry;
	int ret;

	ret = ext4_dir_open(&dir, full);
	if (ret != EOK)
		return ret;

	ext4_dir_entry_rewind(&dir);
	while ((entry = ext4_dir_entry_next(&dir)) != NULL) {
		if (entry->inode == 0 || entry->name_length == 0 ||
		    entry->inode_type == EXT4_DIRENTRY_DIR_CSUM)
			continue;
		if (entry->name_length == 1 && entry->name[0] == '.')
			continue;
		if (entry->name_length == 2 && entry->name[0] == '.' &&
		    entry->name[1] == '.')
			continue;
		ext4_dir_close(&dir);
		return ENOTEMPTY;
	}

	ext4_dir_close(&dir);
	return EOK;
}

static int ext4_parent_dir_check(const char *full)
{
	char *parent = name_get();
	char *slash;
	ext4_dir dir;
	int ret;

	strcpy(parent, full);
	slash = strrchr(parent, '/');
	if (!slash) {
		name_put(parent);
		return ENOENT;
	}

	if (slash == parent)
		parent[1] = '\0';
	else
		*slash = '\0';

	ret = ext4_dir_open(&dir, parent);
	if (ret == EOK)
		ext4_dir_close(&dir);

	name_put(parent);
	return ret;
}

static int ext4_mkdir(super_block *sb, const char *path, unsigned mode)
{
	char *full = name_get();
	unsigned uid = current->user->uid;
	unsigned gid = current->user->gid;
	int ret;
	ext4_dir dir;
	ext4_full_path(sb, path, full);
	ret = ext4_parent_dir_check(full);
	if (ret == EOK)
		ret = ext4_dir_mk(full);
	if (ret == EOK) {
		uint32_t t = (uint32_t)time_now_sec();
		ext4_file_set_mtime(full, t);
		ext4_file_set_ctime(full, t);
		ext4_chown(full, uid, gid);
		if (ext4_dir_open(&dir, full) == EOK) {
			ext4_fchmod(&dir.f, S_IFDIR | (mode & 0777));
			ext4_dir_close(&dir);
		}
	}
	name_put(full);
	return ret ? -ret : 0;
}

static int ext4_rmdir(super_block *sb, const char *path)
{
	char *full = name_get();
	int ret;
	ext4_full_path(sb, path, full);
	ret = ext4_dir_check_empty(full);
	if (ret == EOK)
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

static int ext4_utime(super_block *sb, const char *path, unsigned atime,
		      unsigned mtime)
{
	char *full = name_get();
	ext4_full_path(sb, path, full);
	ext4_file_set_atime(full, atime);
	ext4_file_set_mtime(full, mtime);
	name_put(full);
	return 0;
}

static int ext4_link(super_block *sb, const char *oldpath, const char *newpath)
{
	char *full1 = name_get();
	char *full2 = name_get();
	unsigned uid = current->user->uid;
	unsigned gid = current->user->gid;
	int ret;
	ext4_full_path(sb, oldpath, full1);
	ext4_full_path(sb, newpath, full2);
	ret = ext4_flink(full1, full2);
	if (ret == EOK) {
		ext4_file_set_ctime(full1, (uint32_t)time_now_sec());
		ext4_chown(full2, uid, gid);
	}
	name_put(full1);
	name_put(full2);
	return ret ? -ret : 0;
}

static int ext4_symlink_op(super_block *sb, const char *target,
			   const char *linkpath)
{
	char *full = name_get();
	unsigned uid = current->user->uid;
	unsigned gid = current->user->gid;
	int ret;
	ext4_full_path(sb, linkpath, full);
	ret = ext4_fsymlink(target, full);
	if (ret == EOK) {
		uint32_t t = (uint32_t)time_now_sec();
		ext4_file_set_mtime(full, t);
		ext4_file_set_ctime(full, t);
		ext4_chown(full, uid, gid);
	}
	name_put(full);
	return ret ? -ret : 0;
}

static int ext4_rename(super_block *sb, const char *oldpath,
		       const char *newpath)
{
	char *full1 = name_get();
	char *full2 = name_get();
	unsigned uid = current->user->uid;
	unsigned gid = current->user->gid;
	int ret;
	ext4_full_path(sb, oldpath, full1);
	ext4_full_path(sb, newpath, full2);
	if (ext4_path_is_descendant(full1, full2)) {
		name_put(full1);
		name_put(full2);
		return -EINVAL;
	}
	ret = ext4_frename(full1, full2);
	if (ret == EEXIST) {
		/* POSIX rename(2) must replace the destination if it exists */
		ext4_fremove(full2);
		ret = ext4_frename(full1, full2);
	}
	if (ret == EOK) {
		uint32_t t = (uint32_t)time_now_sec();
		ext4_file_set_mtime(full2, t);
		ext4_file_set_ctime(full2, t);
		ext4_chown(full2, uid, gid);
	}
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

static int ext4_statfs_op(super_block *sb, struct statfs *buf)
{
	ext4_mount_info *mi = sb->s_fs_info;
	struct ext4_mount_stats stats;
	int ret = ext4_mount_point_stats(mi->mp, &stats);

	if (ret != EOK)
		return -EIO;

	memset(buf, 0, sizeof(*buf));
	buf->f_type = 0xEF53; /* EXT4_SUPER_MAGIC */
	buf->f_bsize = stats.block_size;
	buf->f_blocks = stats.blocks_count;
	buf->f_bfree = stats.free_blocks_count;
	buf->f_bavail = stats.free_blocks_count;
	buf->f_files = stats.inodes_count;
	buf->f_ffree = stats.free_inodes_count;
	buf->f_namelen = 255;
	buf->f_frsize = stats.block_size;
	return 0;
}

/* Forward declaration — defined after root_lock below. */
static int ext4_remount(super_block *sb, int flags);

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
	.statfs = ext4_statfs_op,
	.utime = ext4_utime,
	.remount = ext4_remount,
};

/* Allocate a super_block bound to the given lwext4 mount point. */
static super_block *ext4_new_sb(const char *mp, const char *devname)
{
	ext4_mount_info *mi = zalloc(sizeof(*mi));
	strncpy(mi->mp, mp, sizeof(mi->mp) - 1);
	if (devname)
		strncpy(mi->devname, devname, sizeof(mi->devname) - 1);

	super_block *sb = sget(&ext4_sops);
	sb->s_fs_info = mi;
	return sb;
}

/* Root factory: wraps the "/" lwext4 mount set up by fs_mount_root(). */
static super_block *ext4_get(const char *devname)
{
	return ext4_new_sb("/", devname);
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
	blockdev_info bdev;
	const char *dev_name;
	char loop_auto[16]; /* non-empty if we auto-attached a loop device */
	char *mp;
	size_t n;
	bool read_only;
	int ret;
	super_block *sb;

	if (!dev || !target)
		return NULL;

	mp = name_get();
	if (!mp)
		return NULL;

	memset(loop_auto, 0, sizeof(loop_auto));

	if (blockdev_lookup_mountable(dev, &bdev)) {
		dev_name = bdev.name;
	} else {
		const char *ln = loop_setup(dev);
		if (!ln) {
			name_put(mp);
			return NULL;
		}
		strncpy(loop_auto, ln, sizeof(loop_auto) - 1);
		if (!blockdev_lookup_mountable(loop_auto, &bdev)) {
			loop_teardown(loop_auto);
			name_put(mp);
			return NULL;
		}
		dev_name = bdev.name;
	}
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
	if (ret != EOK) {
		if (loop_auto[0])
			loop_teardown(loop_auto);
		name_put(mp);
		return NULL;
	}

	if (!read_only)
		ext4_cache_write_back(mp, true);

	sb = ext4_new_sb(mp, dev_name);
	name_put(mp);

	/* For auto-looped mounts: record for teardown and show /dev/loopN
	 * in /proc/mounts (fs_do_mount won't overwrite a pre-set s_devname). */
	if (loop_auto[0]) {
		ext4_mount_info *mi = sb->s_fs_info;
		strncpy(mi->loop_name, loop_auto, sizeof(mi->loop_name) - 1);
		sprintf(sb->s_devname, "/dev/%s", loop_auto);
	}

	return sb;
}

static fs_type ext4_fs_type = { .name = "ext4", .get_sb = ext4_get_sb };
static fs_type ext3_fs_type = { .name = "ext3", .get_sb = ext4_get_sb };
static fs_type vfat_fs_type = { .name = "vfat", .get_sb = ext4_get_sb };

/* =========================================================================
 * Boot-time root filesystem init
 * ====================================================================== */
static rmutex_t root_lock_;

static void root_lock_lock()
{
	rmutex_lock(&root_lock_);
}

static void root_lock_unlock(void)
{
	rmutex_unlock(&root_lock_);
}

static struct ext4_lock root_lock = {
	.lock = root_lock_lock,
	.unlock = root_lock_unlock,
};

static int ext4_remount(super_block *sb, int flags)
{
	ext4_mount_info *mi = sb->s_fs_info;
	bool rdonly = (flags & MS_RDONLY) != 0;
	int ret;

	ext4_cache_write_back(mi->mp, false);
	ext4_umount(mi->mp);

	ret = ext4_mount(mi->devname, mi->mp, rdonly);
	if (ret != EOK)
		return -EIO;

	ext4_mount_setup_locks(mi->mp, &root_lock);
	if (!rdonly)
		ext4_cache_write_back(mi->mp, true);

	sb->s_flags = (sb->s_flags & ~MS_RDONLY) | (rdonly ? MS_RDONLY : 0);
	return 0;
}

static void fs_mount_root(void)
{
	task_struct *cur = CURRENT_TASK();
	blockdev_info rootdev;
	const char *devname;

	if (!blockdev_first_mountable(&rootdev)) {
		printk("ext4: no discovered root device\n");
		return;
	}
	devname = rootdev.name;

	printk("mnt: Mount rootfs (ro)\n");
	cur->root = ext4_get(devname);
	/* bdev already registered by found_partition at discovery time */
	ext4_mount(devname, "/", true); /* read-only until init remounts rw */
	ext4_mount_setup_locks("/", &root_lock);

	/* Populate root sb metadata for /proc/mounts. */
	sprintf(cur->root->s_devname, "/dev/%s", devname);
	strncpy(cur->root->s_fstype, "ext3", sizeof(cur->root->s_fstype) - 1);
	strncpy(cur->root->s_mountpoint, "/",
		sizeof(cur->root->s_mountpoint) - 1);
	cur->root->s_flags = MS_RDONLY;
}

static void ext_fs_type_init()
{
	printk("mnt: registered ext3 file type\n");
	fs_register_type(&ext3_fs_type);

	printk("mnt: registered ext4 file type\n");
	fs_register_type(&ext4_fs_type);

	printk("mnt: registered vfat file type\n");
	fs_register_type(&vfat_fs_type);
	rmutex_init(&root_lock_);
}

KERNEL_INIT(2, ext_fs_type_init);
KERNEL_INIT(3, fs_mount_root);
