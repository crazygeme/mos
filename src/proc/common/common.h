#ifndef _PROC_ENTRIES_GENERIC_H
#define _PROC_ENTRIES_GENERIC_H

/*
 * DEFINE_PROC_FILE(name, fill_func)
 *
 * Generates the boilerplate needed to expose a single read-only file at
 * /proc/<name>.  The caller provides a fill_func(void *buf, size_t size)
 * that writes the file content into buf (at most PAGE_SIZE bytes).
 *
 * Internally this creates a per-open super_block whose open_root() allocates
 * a page, calls fill_func, and hands the result back as a regular file.
 * The entry is registered under the procfs superblock via PROC_INIT.
 *
 * Usage:
 *   static void fill(void *buf, size_t size) { sprintf(buf, "hello\n"); }
 *   DEFINE_PROC_FILE(hello, fill);
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <proc/proc.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <ext4.h>
#include <stddef.h>

/*
 * proc_buf_t — dynamic string buffer for proc file fill functions.
 *
 * Fill functions write content via proc_buf_printf(); the buffer grows
 * automatically.  The caller owns pb.buf and must kfree() it when done.
 */
typedef struct {
	char *buf;
	size_t len;
	size_t cap;
} proc_buf_t;

proc_buf_t *proc_buf_new(void);

void proc_buf_free(proc_buf_t *pb);

void proc_buf_printf(proc_buf_t *pb, const char *fmt, ...);

void proc_buf_copy(proc_buf_t *pb, const void *src, size_t len);

/* _DEFINE_PROC_FILE_IMPL — all boilerplate except the PROC_INIT registration */
#define _DEFINE_PROC_FILE_IMPL(name, fill_func)                           \
	static int _getattr_##name(file *file, struct stat *s)            \
	{                                                                 \
		inode *inode = file->f_inode;                             \
		unsigned long _now = time_now_sec();                      \
		s->st_atime = _now;                                       \
		s->st_mode = inode->i_mode;                               \
		s->st_size = (loff_t)inode->i_size;                       \
		s->st_blksize = PAGE_SIZE;                                \
		s->st_blocks = 0;                                         \
		s->st_ctime = _now;                                       \
		s->st_dev = 5;                                            \
		s->st_gid = 0;                                            \
		s->st_ino = (unsigned long)inode;                         \
		s->st_mtime = _now;                                       \
		s->st_uid = 0;                                            \
		s->st_nlink = 1;                                          \
		return 0;                                                 \
	}                                                                 \
                                                                          \
	static int _release_##name(file *file)                            \
	{                                                                 \
		proc_buf_free(file->f_inode->i_private);                  \
		free(file->f_inode);                                      \
		free(file);                                               \
		return 0;                                                 \
	}                                                                 \
                                                                          \
	static ssize_t _read_##name(file *file, void *buf, size_t size,   \
				    loff_t *pos)                          \
	{                                                                 \
		loff_t fsize = (loff_t)file->f_inode->i_size;             \
		loff_t offset = *pos;                                     \
		ssize_t left = (ssize_t)(fsize - offset);                 \
		ssize_t read_size = (ssize_t)size > left ? left :         \
							   (ssize_t)size; \
		proc_buf_t *pb = file->f_inode->i_private;                \
		if (read_size <= 0)                                       \
			return 0;                                         \
		memcpy(buf, (char *)pb->buf + offset, (size_t)read_size); \
		*pos = offset + read_size;                                \
		return read_size;                                         \
	}                                                                 \
                                                                          \
	static loff_t _llseek_##name(file *fp, loff_t offset, int whence) \
	{                                                                 \
		loff_t fsize = (loff_t)fp->f_inode->i_size;               \
		loff_t newpos;                                            \
		switch (whence) {                                         \
		case SEEK_SET:                                            \
			newpos = offset;                                  \
			break;                                            \
		case SEEK_CUR:                                            \
			newpos = fp->f_pos + offset;                      \
			break;                                            \
		case SEEK_END:                                            \
			newpos = fsize + offset;                          \
			break;                                            \
		default:                                                  \
			return -EINVAL;                                   \
		}                                                         \
		if (newpos < 0)                                           \
			return -EINVAL;                                   \
		fp->f_pos = newpos;                                       \
		return newpos;                                            \
	}                                                                 \
                                                                          \
	static file_operations _fops_##name = {                           \
		.getattr = _getattr_##name,                               \
		.release = _release_##name,                               \
		.read = _read_##name,                                     \
		.llseek = _llseek_##name,                                 \
	};                                                                \
                                                                          \
	static file *_open_root_##name(super_block *sb, int flag)         \
	{                                                                 \
		proc_buf_t *_pb = proc_buf_new();                         \
		fill_func(_pb);                                           \
		inode *node = zalloc(sizeof(*node));                      \
		node->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;     \
		node->i_private = _pb;                                    \
		node->i_size = _pb->len;                                  \
		file *fp = zalloc(sizeof(*fp));                           \
		fp->f_inode = node;                                       \
		fp->f_count = 1;                                          \
		fp->f_fop = &_fops_##name;                                \
		return fp;                                                \
	}                                                                 \
                                                                          \
	static super_operations _sops_##name = {                          \
		.open_root = _open_root_##name,                           \
	};

/*
 * DEFINE_PROC_FILE(name, fill_func) — register at /proc/<name>
 */
#define DEFINE_PROC_FILE(name, fill_func)                           \
	_DEFINE_PROC_FILE_IMPL(name, fill_func)                     \
                                                                    \
	static void _proc_register_##name(super_block *proc_sb)     \
	{                                                           \
		vfs_mount(proc_sb, "/" #name, sget(&_sops_##name)); \
	}                                                           \
                                                                    \
	PROC_INIT(_proc_register_##name)

/*
 * DEFINE_PROC_FILE_AT(mount_path, name, fill_func)
 *
 * Like DEFINE_PROC_FILE but registers at an explicit path under /proc
 * (e.g. "/sys/kernel/osrelease") instead of "/<name>".
 */
#define DEFINE_PROC_FILE_AT(mount_path, name, fill_func)             \
	_DEFINE_PROC_FILE_IMPL(name, fill_func)                      \
                                                                     \
	static void _proc_register_##name(super_block *proc_sb)      \
	{                                                            \
		vfs_mount(proc_sb, mount_path, sget(&_sops_##name)); \
	}                                                            \
                                                                     \
	PROC_INIT(_proc_register_##name)

#endif /* _PROC_ENTRIES_GENERIC_H */
