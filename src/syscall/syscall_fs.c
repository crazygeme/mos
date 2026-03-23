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
#include <fs/mount.h>
#include <hw/hdd.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <ext4.h>
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

static int do_stat(const char *func, const char *name, struct stat *buf,
		   int follow_link)
{
	file *fp;
	char modes[11];
	int ret = -ENOENT;

	fp = fs_open_file(name, 0, "r", follow_link);
	if (!fp)
		goto done;
	if (!fp->f_inode || !fp->f_inode->i_op || !fp->f_inode->i_op->getattr)
		goto done;

	ret = fp->f_inode->i_op->getattr(fp->f_inode, buf);
	if (TestControl.verbos && func) {
		format_modes(buf->st_mode, modes);
		klog("%s(%s, %x) = %d, %s, size=%d, blocks = %d, ino = %d, rdev = %d, dev = %d, nlink = %d\n",
		     func, name, buf, ret, modes, buf->st_size, buf->st_blocks,
		     buf->st_ino, buf->st_rdev, buf->st_dev, buf->st_nlink);
	}

done:
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

	resolve_path(_name, name);
	ret = do_stat("stat", name, buf, 1);
	name_put(name);
	return ret;
}

int sys_lstat(const char *_name, struct stat *buf)
{
	char *name = name_get();
	int ret;

	resolve_path(_name, name);
	ret = do_stat("lstat", name, buf, 0);
	name_put(name);
	return ret;
}

int sys_fstat(int fd, struct stat *buf)
{
	int ret = fs_fstat(fd, buf);

	if (TestControl.verbos) {
		char modes[11];
		format_modes(buf->st_mode, modes);
		klog("fstat(%d, %x) = %d, %s, size=%d, blocks = %d, ino = %d, rdev = %d, dev = %d, nlink = %d\n",
		     fd, buf, ret, modes, buf->st_size, buf->st_blocks,
		     buf->st_ino, buf->st_rdev, buf->st_dev, buf->st_nlink);
	}

	return ret;
}

int sys_stat64(const char *path, struct stat64 *s)
{
	struct stat s32;
	char *name = name_get();
	int ret;

	resolve_path(path, name);
	ret = do_stat("stat64", name, &s32, 1);
	memset(s, 0, sizeof(*s));
	s->st_dev = s32.st_dev;
	s->__pad0 = 0;
	s->st_ino = s32.st_ino;
	s->st_mode = s32.st_mode;
	s->st_nlink = s32.st_nlink;
	s->st_uid = s32.st_uid;
	s->st_gid = s32.st_gid;
	s->st_rdev = s32.st_rdev;
	s->st_size = s32.st_size;
	s->st_blksize = s32.st_blksize;
	s->st_blocks = s32.st_blocks;
	s->st_atime = s32.st_atime;
	s->st_mtime = s32.st_mtime;
	s->st_ctime = s32.st_ctime;
	name_put(name);
	return ret;
}

int sys_lstat64(const char *path, struct stat64 *s)
{
	struct stat s32;
	char *name = name_get();
	int ret;

	resolve_path(path, name);
	ret = do_stat("lstat64", name, &s32, 0);
	memset(s, 0, sizeof(*s));
	s->st_dev = s32.st_dev;
	s->__pad0 = 0;
	s->st_ino = s32.st_ino;
	s->st_mode = s32.st_mode;
	s->st_nlink = s32.st_nlink;
	s->st_uid = s32.st_uid;
	s->st_gid = s32.st_gid;
	s->st_rdev = s32.st_rdev;
	s->st_size = s32.st_size;
	s->st_blksize = s32.st_blksize;
	s->st_blocks = s32.st_blocks;
	s->st_atime = s32.st_atime;
	s->st_mtime = s32.st_mtime;
	s->st_ctime = s32.st_ctime;
	name_put(name);
	return ret;
}

int sys_fstat64(int fd, struct stat64 *buf)
{
	struct stat s32;
	int ret;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	ret = fs_fstat(fd, &s32);
	buf->st_dev = s32.st_dev;
	buf->st_ino = s32.st_ino;
	buf->st_mode = s32.st_mode;
	buf->st_nlink = s32.st_nlink;
	buf->st_uid = s32.st_uid;
	buf->st_gid = s32.st_gid;
	buf->st_rdev = s32.st_rdev;
	buf->st_size = s32.st_size;
	buf->st_blksize = s32.st_blksize;
	buf->st_blocks = s32.st_blocks;
	buf->st_atime = s32.st_atime;
	buf->st_mtime = s32.st_mtime;
	buf->st_ctime = s32.st_ctime;

	if (TestControl.verbos) {
		char modes[11];
		format_modes(buf->st_mode, modes);
		klog("fstat64(%d, %x) = %d, %s, size=%d, blocks = %d, ino = %d, rdev = %d, dev = %d, nlink = %d\n",
		     fd, buf, ret, modes, buf->st_size, buf->st_blocks,
		     buf->st_ino, buf->st_rdev, buf->st_dev, buf->st_nlink);
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
 * Permissions and access                                               *
 * ------------------------------------------------------------------ */

int sys_access(const char *path, int mode)
{
	struct stat s;
	char *name = name_get();
	int ret = -EACCES;

	resolve_path(path, name);
	ret = do_stat(NULL, name, &s, 1);

	if (ret != EOK) {
		ret = -ENOENT;
		goto done;
	}

	if (mode == F_OK)
		goto done;

	ret = EOK;
	if (mode & R_OK)
		ret |= ((s.st_mode & S_IRUSR) == S_IRUSR) ? EOK : (-EACCES);
	if (mode & W_OK)
		ret |= ((s.st_mode & S_IWUSR) == S_IWUSR) ? EOK : (-EACCES);
	if (mode & X_OK)
		ret |= ((s.st_mode & S_IXUSR) == S_IXUSR) ? EOK : (-EACCES);

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
	file *fp;
	int ret;

	resolve_path(pathname, name);

	/* Open without following the final symlink, then chown the inode. */
	fp = fs_open_file(name, 0, "r", 0);
	if (!fp) {
		name_put(name);
		return -ENOENT;
	}

	ret = ext4_fchown(fp->f_inode->i_private, uid, gid);
	ret = (0 - ret);
	fs_put_file(fp);
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

int sys_mkdir(const char *path, unsigned mode)
{
	char *name = name_get();
	task_struct *cur = CURRENT_TASK();
	int ret;

	resolve_path(path, name);
	ret = vfs_mkdir(cur->root, name, mode);

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
	if (TestControl.verbos)
		klog("creat(%s, %d)\n", path, mode);

	return -1;
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

int sys_unlink(const char *_name)
{
	char *name = name_get();
	task_struct *cur = CURRENT_TASK();
	struct stat s;
	int ret;

	resolve_path(_name, name);
	ret = do_stat(NULL, name, &s, 0);
	if (ret != EOK)
		goto done;

	if (S_ISDIR(s.st_mode)) {
		ret = -EISDIR;
		goto done;
	}

	ret = vfs_unlink(cur->root, name);
done:
	if (TestControl.verbos)
		klog("unlink(%s) = %d\n", name, ret);
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

	if (TestControl.verbos)
		klog("readlink(%s, %x, %d)\n", name, buf, bufsiz);

	ret = vfs_readlink(cur->root, name, buf, bufsiz, &rcnt);
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

	hdd_flush();
	return 0;
}

int sys_chdir(const char *path)
{
	task_struct *cur = CURRENT_TASK();
	char *cwd = name_get();
	const char *p;
	struct stat s;
	int ret = 0;

	if (!path || !*path) {
		ret = -ENOENT;
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
			cwd[len + comp_len] = '/';
			cwd[len + comp_len + 1] = '\0';
			if (do_stat(NULL, cwd, &s, 1) != EOK) {
				ret = -ENOENT;
				goto done;
			}
			if (!S_ISDIR(s.st_mode)) {
				ret = -ENOTDIR;
				goto done;
			}
		}
		p = end;
	}

	strcpy(cur->user->cwd, cwd);
done:
	name_put(cwd);
	return ret;
}
