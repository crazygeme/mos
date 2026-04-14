/*
 * syscall_fs.c — filesystem and path syscall handlers.
 *
 * Covers: stat family, access, chmod, link, symlink, unlink,
 *         rename, mkdir, rmdir, creat, mount, umount, readlink,
 *         sync, chdir.
 */

#include <ps/ps.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
#include <hw/hdd.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <dev/dev.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include "syscall_internal.h"

/* ------------------------------------------------------------------ *
 * Shared helpers                                                       *
 * ------------------------------------------------------------------ */

static int format_modes(unsigned mode, char *str)
{
	memset(str, '-', 11);
	str[0] = '-';
	if (S_ISDIR(mode))
		str[0] = 'd';
	else if (S_ISCHR(mode))
		str[0] = 'c';
	else if (S_ISLNK(mode))
		str[0] = 'l';
	else if (S_ISFIFO(mode))
		str[0] = 'p';

	if (mode & S_IRUSR)
		str[1] = 'r';
	if (mode & S_IWUSR)
		str[2] = 'w';
	if (mode & S_IXUSR)
		str[3] = 'x';
	if (mode & S_IRGRP)
		str[4] = 'r';
	if (mode & S_IWGRP)
		str[5] = 'w';
	if (mode & S_IXGRP)
		str[6] = 'x';
	if (mode & S_IROTH)
		str[7] = 'r';
	if (mode & S_IWOTH)
		str[8] = 'w';
	if (mode & S_IXOTH)
		str[9] = 'x';

	str[10] = '\0';
	return 0;
}

static void trim_trailing_slash(char *path)
{
	int len = strlen(path);

	while (len > 1 && path[len - 1] == '/') {
		path[len - 1] = '\0';
		len--;
	}
}

static int rooted_path_to_cwd(task_struct *cur, const char *path, char *cwd)
{
	const char *root_path = "/";
	int root_len;

	if (cur && cur->user && cur->user->root_path && cur->user->root_path[0])
		root_path = cur->user->root_path;

	if (!strcmp(root_path, "/")) {
		strcpy(cwd, path);
		trim_trailing_slash(cwd);
		return 0;
	}

	root_len = strlen(root_path);
	if (strncmp(path, root_path, root_len) != 0)
		return -ENOENT;

	if (path[root_len] == '\0') {
		strcpy(cwd, "/");
		return 0;
	}
	if (path[root_len] != '/')
		return -ENOENT;

	strcpy(cwd, path + root_len);
	trim_trailing_slash(cwd);
	return 0;
}

typedef struct {
	const char *path;
	const char *newpath;
	int found;
} open_path_ctx;

static void mark_open_unlinked_files(task_struct *task, void *opaque)
{
	open_path_ctx *ctx = opaque;
	int i;

	if (!task || !task->fds)
		return;

	for (i = 0; i < MAX_FD; i++) {
		file *fp = task->fds[i];

		if (!fp || !fp->f_name || strcmp(fp->f_name, ctx->path) != 0)
			continue;
		ctx->found = 1;
		if (ctx->newpath) {
			free(fp->f_name);
			fp->f_name = strdup(ctx->newpath);
			fp->f_state |= FS_FILE_UNLINK_ON_CLOSE;
		}
	}
}

static int unlink_open_file(task_struct *cur, const char *path)
{
	static unsigned tombstone_seq;
	open_path_ctx ctx;
	char *newpath;
	const char *slash;
	unsigned seq;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.path = path;
	ps_enum_all(mark_open_unlinked_files, &ctx);
	if (!ctx.found)
		return -ENOENT;

	newpath = name_get();
	slash = strrchr(path, '/');
	seq = ++tombstone_seq;

	if (!slash || slash == path) {
		sprintf(newpath, "/.mos-unlinked-%u-%u", CURRENT_TASK()->psid,
			seq);
	} else {
		unsigned dir_len = (unsigned)(slash - path);

		memcpy(newpath, path, dir_len);
		newpath[dir_len] = '\0';
		sprintf(newpath + dir_len, "/.mos-unlinked-%u-%u",
			CURRENT_TASK()->psid, seq);
	}

	ret = vfs_rename(cur->root, path, newpath);
	if (ret == 0) {
		ctx.newpath = newpath;
		ps_enum_all(mark_open_unlinked_files, &ctx);
	}

	name_put(newpath);
	return ret;
}

static int do_stat(const char *func, const char *name, struct stat *buf,
		   int flag)
{
	file *fp = NULL;
	char modes[11];
	int ret = -ENOENT;

	memset(buf, 0, sizeof(*buf));
	fp = fs_open_file(name, flag, 0);
	if (!fp)
		goto log;
	if (!fp->f_inode || !fp->f_inode->i_op || !fp->f_inode->i_op->getattr)
		goto log;
	ret = fp->f_inode->i_op->getattr(fp->f_inode, buf);

log:
	if (TestControl.verbos && func) {
		format_modes(buf->st_mode, modes);
		klog("%s(%s, %x) = %d, %s, uid=%d, gid=%d, size=%d, blocks = %d, ino = %d, rdev = %d (%d:%d), dev = %d (%d:%d), nlink = %d\n",
		     func, name, buf, ret, modes, buf->st_uid, buf->st_gid,
		     buf->st_size, buf->st_blocks, buf->st_ino, buf->st_rdev,
		     MAJOR(buf->st_rdev), MINOR(buf->st_rdev), buf->st_dev,
		     MAJOR(buf->st_dev), MINOR(buf->st_dev), buf->st_nlink);
	}
	if (fp)
		fs_put_file(fp);
	return ret;
}

/* ------------------------------------------------------------------ *
 * stat family                                                          *
 * ------------------------------------------------------------------ */

int sys_stat(const char *_name, struct stat *buf)
{
	char *name = name_get();
	int ret;

	ret = resolve_path(_name, name);
	if (ret == 0)
		ret = do_stat("stat", name, buf, O_PATH);
	else
		ret = -ENOENT;
	name_put(name);
	return ret;
}

int sys_lstat(const char *_name, struct stat *buf)
{
	char *name = name_get();
	int ret;

	ret = resolve_path(_name, name);
	if (ret == 0)
		ret = do_stat("lstat", name, buf, O_PATH | O_NOFOLLOW);
	else
		ret = -ENOENT;
	name_put(name);
	return ret;
}

int sys_fstat(int fd, struct stat *buf)
{
	int ret = fs_fstat(fd, buf);

	if (TEST_LOG(TEST_LOG_INFO)) {
		char modes[11];
		format_modes(buf->st_mode, modes);
		klog("fstat(%d, %x) = %d, %s, uid=%d, gid=%d, size=%d, blocks = %d, ino = %d, rdev = %d (%d:%d), dev = %d (%d:%d), nlink = %d\n",
		     fd, buf, ret, modes, buf->st_uid, buf->st_gid,
		     buf->st_size, buf->st_blocks, buf->st_ino, buf->st_rdev,
		     MAJOR(buf->st_rdev), MINOR(buf->st_rdev), buf->st_dev,
		     MAJOR(buf->st_dev), MINOR(buf->st_dev), buf->st_nlink);
	}

	return ret;
}

static void stat_to_stat64(const struct stat *s32, struct stat64 *s)
{
	s->st_dev = s32->st_dev;
	s->__st_ino = s32->st_ino;
	s->st_mode = s32->st_mode;
	s->st_nlink = s32->st_nlink;
	s->st_uid = s32->st_uid;
	s->st_gid = s32->st_gid;
	s->st_rdev = s32->st_rdev;
	s->st_size = s32->st_size;
	s->st_blksize = s32->st_blksize;
	s->st_blocks = s32->st_blocks;
	s->st_atime = s32->st_atime;
	s->st_mtime = s32->st_mtime;
	s->st_ctime = s32->st_ctime;
	s->st_ino = s32->st_ino;
}

int sys_stat64(const char *path, struct stat64 *s)
{
	struct stat s32;
	char *name = name_get();
	int ret;

	ret = resolve_path(path, name);
	if (ret == 0)
		ret = do_stat("stat64", name, &s32, O_PATH);
	else
		ret = -ENOENT;
	memset(s, 0, sizeof(*s));
	if (ret == 0)
		stat_to_stat64(&s32, s);

	name_put(name);
	return ret;
}

int sys_lstat64(const char *path, struct stat64 *s)
{
	struct stat s32;
	char *name = name_get();
	int ret;

	ret = resolve_path(path, name);
	if (ret == 0)
		ret = do_stat("lstat64", name, &s32, O_PATH | O_NOFOLLOW);
	else
		ret = -ENOENT;
	memset(s, 0, sizeof(*s));
	if (ret == 0)
		stat_to_stat64(&s32, s);

	name_put(name);
	return ret;
}

int sys_fstat64(int fd, struct stat64 *buf)
{
	struct stat s32;
	int ret;

	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;

	ret = fs_fstat(fd, &s32);
	memset(buf, 0, sizeof(*buf));
	if (ret == 0)
		stat_to_stat64(&s32, buf);

	if (TEST_LOG(TEST_LOG_INFO)) {
		char modes[11];
		format_modes(buf->st_mode, modes);
		klog("fstat64(%d, %x) = %d, %s, uid=%d, gid=%d, size=%d, blocks = %d, ino = %d, rdev = %d (%d:%d), dev = %d (%d:%d), nlink = %d\n",
		     fd, buf, ret, modes, buf->st_uid, buf->st_gid,
		     buf->st_size, buf->st_blocks, buf->st_ino, buf->st_rdev,
		     MAJOR(buf->st_rdev), MINOR(buf->st_rdev), buf->st_dev,
		     MAJOR(buf->st_dev), MINOR(buf->st_dev), buf->st_nlink);
	}

	return ret;
}

int sys_oldstat(const char *filename, struct oldstat *buf)
{
	struct stat s;
	int ret = sys_stat(filename, &s);

	if (ret != EOK)
		return ret;

	buf->st_dev = s.st_dev;
	buf->st_ino = s.st_ino;
	buf->st_mode = s.st_mode;
	buf->st_nlink = s.st_nlink;
	buf->st_uid = s.st_uid;
	buf->st_gid = s.st_gid;
	buf->st_rdev = s.st_rdev;
	buf->st_size = s.st_size;
	buf->st_atime = s.st_atime;
	buf->st_mtime = s.st_mtime;
	buf->st_ctime = s.st_ctime;
	return 0;
}

/* ------------------------------------------------------------------ *
 * statfs / fstatfs                                                     *
 * ------------------------------------------------------------------ */

int sys_statfs(const char *_path, struct statfs *buf)
{
	char *name = name_get();
	int ret;

	resolve_path(_path, name);
	ret = vfs_statfs(CURRENT_TASK()->root, name, buf);

	if (TestControl.verbos)
		klog("statfs(%s, %x) = %d, type=%x, bsize=%d, namelen=%d\n",
		     name, buf, ret, buf->f_type, buf->f_bsize, buf->f_namelen);

	name_put(name);
	return ret;
}

int sys_fstatfs(int fd, struct statfs *buf)
{
	task_struct *cur = CURRENT_TASK();
	file *fp;
	int ret;

	if (fd < 0 || fd >= (int)MAX_FD)
		return -EBADF;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd];
	mutex_unlock(&cur->fd_lock);

	if (!fp)
		return -EBADF;
	if (!fp->f_name)
		return -ENOSYS;

	ret = vfs_statfs(cur->root, fp->f_name, buf);

	if (TestControl.verbos)
		klog("fstatfs(%d, %x) = %d, path=%s, type=%x, bsize=%d, namelen=%d\n",
		     fd, buf, ret, fp->f_name, buf->f_type, buf->f_bsize,
		     buf->f_namelen);

	return ret;
}

/* ------------------------------------------------------------------ *
 * Permissions and access                                               *
 * ------------------------------------------------------------------ */

int sys_access(const char *path, int mode)
{
	struct stat s;
	char *name = name_get();
	int ret = -EACCES;

	resolve_path(path, name);
	ret = do_stat(NULL, name, &s, O_PATH);

	if (ret != EOK) {
		ret = -ENOENT;
		goto done;
	}

	if (mode == F_OK)
		goto done;

	ret = fs_check_perm(&s, mode);

done:
	name_put(name);
	if (TestControl.verbos)
		klog("access(%s, %x) = %d\n", path, mode, ret);
	return ret;
}

int sys_chmod(const char *pathname, uint32_t mode)
{
	char *name = name_get();
	int ret;

	resolve_path(pathname, name);
	ret = fs_chmod(name, mode);
	name_put(name);

	if (TestControl.verbos)
		klog("chmod(%s, %d) = %d\n", pathname, mode, ret);

	return ret;
}

int sys_fchmod(int fd, uint32_t mode)
{
	int ret = fs_fchmod(fd, mode);

	if (TestControl.verbos)
		klog("fchmod(%d, %d) = %d\n", fd, mode, ret);

	return ret;
}

int sys_chown(const char *pathname, uint32_t uid, uint32_t gid)
{
	char *name = name_get();
	int ret;

	resolve_path(pathname, name);

	ret = fs_chown(name, uid, gid);
	name_put(name);

	if (TestControl.verbos)
		klog("chown(%s, %d, %d) = %d\n", pathname, uid, gid, ret);

	return ret;
}

int sys_lchown(const char *pathname, uint32_t uid, uint32_t gid)
{
	char *name = name_get();
	int ret;

	resolve_path(pathname, name);

	ret = fs_chown(name, uid, gid);
	name_put(name);

	if (TestControl.verbos)
		klog("lchown(%s, %d, %d) = %d\n", pathname, uid, gid, ret);

	return ret;
}

int sys_fchown(int fd, uint32_t uid, uint32_t gid)
{
	int ret = fs_fchown(fd, uid, gid);

	if (TestControl.verbos)
		klog("fchown(%d, %d, %d) = %d\n", fd, uid, gid, ret);

	return ret;
}

/* ------------------------------------------------------------------ *
 * Directory and link operations                                        *
 * ------------------------------------------------------------------ */

int sys_mknod(const char *_path, unsigned mode, unsigned dev)
{
	task_struct *cur = CURRENT_TASK();
	char *path = name_get();
	int ret;

	resolve_path(_path, path);
	ret = vfs_mknod(cur->root, path, mode, dev);

	if (TestControl.verbos)
		klog("mknod(%s, %o, %x) = %d\n", _path, mode, dev, ret);

	name_put(path);
	return ret;
}

int sys_mkdir(const char *path, unsigned mode)
{
	char *name = name_get();
	task_struct *cur = CURRENT_TASK();
	int ret;
	unsigned masked_mode;

	resolve_path(path, name);
	masked_mode = mode & ~(cur->umask & 0777U);
	ret = vfs_mkdir(cur->root, name, masked_mode);

	if (TestControl.verbos)
		klog("mkdir(%s, %d) = %d\n", name, mode, ret);

	name_put(name);
	return ret;
}

int sys_rmdir(const char *path)
{
	char *name = name_get();
	task_struct *cur = CURRENT_TASK();
	int ret;

	resolve_path(path, name);
	ret = vfs_rmdir(cur->root, name);

	if (TestControl.verbos)
		klog("rmdir(%s) = %d\n", name, ret);

	name_put(name);
	return ret;
}

int sys_creat(const char *path, unsigned mode)
{
	int fd;
	if (TestControl.verbos)
		klog("creat(%s, %d)\n", path, mode);

	fd = fs_open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
	if (fd < 0)
		return fd;
	return fs_close(fd);
}

int sys_link(const char *path1, const char *path2)
{
	char *name1 = name_get();
	char *name2 = name_get();
	task_struct *cur = CURRENT_TASK();
	int ret;

	resolve_path(path1, name1);
	resolve_path(path2, name2);
	ret = vfs_link(cur->root, name1, name2);

	if (TestControl.verbos)
		klog("link(%s, %s) = %d\n", name1, name2, ret);

	name_put(name1);
	name_put(name2);
	return ret;
}

int sys_symlink(const char *path1, const char *path2)
{
	char *name2 = name_get();
	task_struct *cur = CURRENT_TASK();
	int ret;

	/* path1 is the symlink target — store verbatim, do not resolve */
	resolve_path(path2, name2);
	ret = vfs_symlink(cur->root, path1, name2);

	if (TestControl.verbos)
		klog("symlink(%s, %s) = %d\n", path1, name2, ret);

	name_put(name2);
	return ret;
}

/* defined in src/net/sock_un.c */
void unix_ns_remove_path(const char *path);

int sys_unlink(const char *_name)
{
	char *name = name_get();
	task_struct *cur = CURRENT_TASK();
	struct stat s;
	int ret;

	resolve_path(_name, name);
	ret = do_stat(NULL, name, &s, O_PATH | O_NOFOLLOW);
	if (ret != EOK)
		goto done;

	if (S_ISDIR(s.st_mode)) {
		ret = -EISDIR;
		goto done;
	}

	if (S_ISSOCK(s.st_mode)) {
		unix_ns_remove_path(name);
		ret = vfs_umount(cur->root, name);
		goto done;
	}

	ret = unlink_open_file(cur, name);
	if (ret == -ENOENT)
		ret = vfs_unlink(cur->root, name);
done:
	if (TestControl.verbos)
		klog("unlink(%s) = %d\n", name, ret);
	name_put(name);
	return ret;
}

int sys_utime(const char *filename, const struct utimbuf *times)
{
	unsigned atime, mtime;
	char *name;
	task_struct *cur;
	int ret;

	if (!filename)
		return -EFAULT;

	if (times) {
		atime = times->actime;
		mtime = times->modtime;
	} else {
		atime = mtime = (unsigned)time_now_sec();
	}

	name = name_get();
	cur = CURRENT_TASK();
	resolve_path(filename, name);

	if (TestControl.verbos)
		klog("utime(%s, atime=%u mtime=%u)\n", name, atime, mtime);

	ret = vfs_utime(cur->root, name, atime, mtime);
	name_put(name);
	return ret;
}

int sys_rename(const char *oldpath, const char *newpath)
{
	char *name1 = name_get();
	char *name2 = name_get();
	task_struct *cur = CURRENT_TASK();
	int ret;

	if (!oldpath || !*oldpath || !newpath || !*newpath) {
		name_put(name1);
		name_put(name2);
		return -ENOENT;
	}

	resolve_path(oldpath, name1);
	resolve_path(newpath, name2);
	ret = vfs_rename(cur->root, name1, name2);

	if (TestControl.verbos)
		klog("rename(%s, %s) = %d\n", name1, name2, ret);

	name_put(name1);
	name_put(name2);
	return ret;
}

int sys_readlink(const char *_path, char *buf, unsigned bufsiz)
{
	char *name = name_get();
	task_struct *cur = CURRENT_TASK();
	size_t rcnt = 0;
	int ret;

	if (!buf || bufsiz == 0) {
		name_put(name);
		return -EINVAL;
	}

	resolve_path(_path, name);

	ret = vfs_readlink(cur->root, name, buf, bufsiz, &rcnt);
	if (TestControl.verbos)
		klog("readlink(%s, %s, %d) = %d\n", name, buf, bufsiz,
		     ret ? ret : rcnt);

	name_put(name);

	if (ret)
		return ret;

	return (int)rcnt;
}

/* ------------------------------------------------------------------ *
 * Mount, sync, and cwd                                                 *
 * ------------------------------------------------------------------ */

int sys_mount(char *dev, char *dir_name, char *type, unsigned flag, void *data)
{
	int ret = fs_do_mount(dev, dir_name, type, flag, data);

	if (TestControl.verbos)
		klog("mount(%s, %s, %s, %d, %x) = %d\n", dev, dir_name, type,
		     flag, data, ret);

	return ret;
}

int sys_umount(char *name, int flag)
{
	int ret = fs_do_umount(name, flag);

	if (TestControl.verbos)
		klog("umount(%s, %d) = %d\n", name, flag, ret);

	return ret;
}

int sys_sync()
{
	if (TestControl.verbos)
		klog("sync()\n");

	if (TestControl.test)
		return 0;

	fs_sync_super(current->root);
	hdd_flush();
	return 0;
}

int sys_chdir(const char *path)
{
	task_struct *cur = CURRENT_TASK();
	char *cwd = name_get();
	char *full = name_get();
	const char *p;
	struct stat s;
	int ret = 0;

	if (!path || !*path) {
		ret = -ENOENT;
		goto done;
	}
	if (!cur || !cur->user || !cur->user->cwd) {
		ret = -EINVAL;
		goto done;
	}

	if (TestControl.verbos)
		klog("chdir(%s)\n", path);

	if (*path == '/') {
		cwd[0] = '/';
		cwd[1] = '\0';
		p = path + 1;
	} else {
		int len;
		strcpy(cwd, cur->user->cwd);
		len = strlen(cwd);
		if (!len || cwd[len - 1] != '/')
			strcat(cwd, "/");
		p = path;
	}

	while (*p) {
		const char *end;
		int comp_len, len;

		while (*p == '/')
			p++;
		if (!*p)
			break;

		end = p;
		while (*end && *end != '/')
			end++;
		comp_len = end - p;

		if (comp_len == 1 && p[0] == '.') {
			/* '.' — stay */
		} else if (comp_len == 2 && p[0] == '.' && p[1] == '.') {
			len = strlen(cwd);
			if (len > 1 && cwd[len - 1] == '/')
				cwd[--len] = '\0';
			char *sep = strrchr(cwd, '/');
			if (sep == cwd)
				cwd[1] = '\0';
			else if (sep)
				*sep = '\0';
			len = strlen(cwd);
			if (!len || cwd[len - 1] != '/')
				strcat(cwd, "/");
		} else {
			len = strlen(cwd);
			memcpy(cwd + len, p, comp_len);
			cwd[len + comp_len] =
				'\0'; /* no trailing slash: allows symlink following */
			resolve_path(cwd, full);
			if (do_stat(NULL, full, &s, O_PATH) != EOK) {
				ret = -ENOENT;
				goto done;
			}
			if (!S_ISDIR(s.st_mode)) {
				ret = -ENOTDIR;
				goto done;
			}
			cwd[len + comp_len] = '/';
			cwd[len + comp_len + 1] = '\0';
		}
		p = end;
	}

	trim_trailing_slash(cwd);
	strcpy(cur->user->cwd, cwd);
done:
	name_put(full);
	name_put(cwd);
	return ret;
}

int sys_fchdir(int fd)
{
	task_struct *cur = CURRENT_TASK();
	struct stat s;
	file *fp;

	if (!cur || !cur->user || !cur->user->cwd)
		return -EINVAL;

	if (fd < 0 || fd >= (int)MAX_FD)
		return -EBADF;
	if (!cur->fds[fd])
		return -EBADF;
	if (fs_fstat(fd, &s) != EOK)
		return -EBADF;
	if (!S_ISDIR(s.st_mode))
		return -ENOTDIR;

	fp = cur->fds[fd];
	if (!fp || !fp->f_name)
		return -EBADF;
	{
		char *cwd = name_get();
		int ret = rooted_path_to_cwd(cur, fp->f_name, cwd);
		if (ret == 0)
			strcpy(cur->user->cwd, cwd);
		name_put(cwd);
		return ret;
	}
}

int sys_chroot(const char *path)
{
	task_struct *cur = CURRENT_TASK();
	char *name = name_get();
	struct stat s;
	int ret;

	if (!path || !*path) {
		ret = -ENOENT;
		goto done;
	}
	if (!cur || !cur->user || !cur->user->cwd || !cur->user->root_path) {
		ret = -EINVAL;
		goto done;
	}

	if (cur->user->euid != 0) {
		ret = -EPERM;
		goto done;
	}

	if (TestControl.verbos)
		klog("chroot(%s)\n", path);

	resolve_path(path, name);
	ret = do_stat(NULL, name, &s, O_PATH);
	if (ret != EOK) {
		ret = -ENOENT;
		goto done;
	}
	if (!S_ISDIR(s.st_mode)) {
		ret = -ENOTDIR;
		goto done;
	}

	trim_trailing_slash(name);
	strcpy(cur->user->root_path, name[0] ? name : "/");
	strcpy(cur->user->cwd, "/");
	ret = 0;

done:
	name_put(name);
	return ret;
}

int sys_flock(int fd, int operation)
{
	task_struct *cur = CURRENT_TASK();
	file *fp;
	inode *in;
	int op_type = operation & ~LOCK_NB;
	int nonblock = operation & LOCK_NB;
	int irq, ret = 0;
	int conflict;
	int other_sh;

	if (TestControl.verbos)
		klog("flock(%d, %d)\n", fd, operation);

	if (fd < 0 || fd >= (int)MAX_FD || !cur->fds[fd])
		return -EBADF;

	fp = cur->fds[fd];
	in = fp->f_inode;

	if (!in)
		return -EINVAL;

	/* Lazy-init: first caller initialises the inode's flock fields. */
	if (!in->i_flock_inited) {
		spinlock_init(&in->i_flock_lock);
		list_init(&in->i_flock_wait);
		in->i_flock_inited = 1;
	}

	spinlock_lock(&in->i_flock_lock, &irq);

	if (op_type == LOCK_UN) {
		if (fp->f_flock == LOCK_SH)
			in->i_flock_sh--;
		else if (fp->f_flock == LOCK_EX)
			in->i_flock_ex_owner = NULL;
		fp->f_flock = 0;
		flock_wake_all_locked(in);
		goto out;
	}

	if (op_type != LOCK_SH && op_type != LOCK_EX) {
		ret = -EINVAL;
		goto out;
	}

retry:
	if (op_type == LOCK_SH) {
		/* Conflict: someone else holds EX */
		conflict = (in->i_flock_ex_owner != NULL &&
			    in->i_flock_ex_owner != fp);
	} else {
		/* Conflict: someone else holds EX, or other SH holders exist */
		other_sh = in->i_flock_sh - (fp->f_flock == LOCK_SH ? 1 : 0);
		conflict = (in->i_flock_ex_owner != NULL &&
			    in->i_flock_ex_owner != fp) ||
			   (other_sh > 0);
	}

	if (conflict) {
		if (nonblock) {
			ret = -EWOULDBLOCK;
			goto out;
		}
		ps_put_to_wait_queue(cur, &in->i_flock_wait, __func__);
		spinlock_unlock(&in->i_flock_lock, irq);
		task_sched();
		spinlock_lock(&in->i_flock_lock, &irq);
		goto retry;
	}

	/* Release whatever lock we currently hold before acquiring the new one. */
	if (fp->f_flock == LOCK_SH)
		in->i_flock_sh--;
	else if (fp->f_flock == LOCK_EX)
		in->i_flock_ex_owner = NULL;

	/* Acquire the requested lock. */
	if (op_type == LOCK_SH) {
		in->i_flock_sh++;
		fp->f_flock = LOCK_SH;
	} else {
		in->i_flock_ex_owner = fp;
		fp->f_flock = LOCK_EX;
	}

out:
	spinlock_unlock(&in->i_flock_lock, irq);
	return ret;
}

int sys_ftruncate(int fd, unsigned long length)
{
	task_struct *cur = CURRENT_TASK();
	file *fp;
	int ret;

	if (TestControl.verbos)
		klog("ftruncate(%d, %lu)\n", fd, length);

	if (fd < 0 || fd >= MAX_FD || !cur->fds[fd])
		return -EBADF;

	fp = cur->fds[fd];
	if (!fp->f_inode->i_op || !fp->f_inode->i_op->ftruncate)
		return -EINVAL;

	ret = fp->f_inode->i_op->ftruncate(fp->f_inode, (loff_t)length);
	if (ret == 0)
		fp->f_inode->i_size = length;
	return ret;
}

int sys_ftruncate64(int fd, uint64_t length)
{
	task_struct *cur = CURRENT_TASK();
	file *fp;
	int ret;

	if (TestControl.verbos)
		klog("ftruncate64(%d, %llu)\n", fd, length);

	if (fd < 0 || fd >= MAX_FD || !cur->fds[fd])
		return -EBADF;

	fp = cur->fds[fd];
	if (!fp->f_inode->i_op || !fp->f_inode->i_op->ftruncate)
		return -EINVAL;

	ret = fp->f_inode->i_op->ftruncate(fp->f_inode, (loff_t)length);
	if (ret == 0)
		fp->f_inode->i_size = length;
	return ret;
}

int sys__sysctl(void *args)
{
	if (TestControl.verbos)
		klog("_sysctl\n");

	return -ENOSYS;
}
