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
#include <errno.h>
#include <ext4.h>

#define DEFINE_PROC_FILE(name, fill_func)                                 \
	static int _getattr_##name(inode *inode, struct stat *s)          \
	{                                                                 \
		s->st_atime = time_now_ms();                              \
		s->st_mode = inode->i_mode;                               \
		s->st_size = (loff_t)inode->i_size;                       \
		s->st_blksize = PAGE_SIZE;                                \
		s->st_blocks = 0;                                         \
		s->st_ctime = time_now_ms();                              \
		s->st_dev = 0;                                            \
		s->st_gid = 0;                                            \
		s->st_ino = 0;                                            \
		s->st_mtime = 0;                                          \
		s->st_uid = 0;                                            \
		return 0;                                                 \
	}                                                                 \
                                                                          \
	static int _release_##name(file *file)                            \
	{                                                                 \
		vm_free(file->f_inode->i_private, 1);                     \
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
		if (read_size <= 0)                                       \
			return 0;                                         \
		memcpy(buf, (char *)file->f_inode->i_private + offset,    \
		       (size_t)read_size);                                \
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
	static inode_operations _iops_##name = {                          \
		.getattr = _getattr_##name,                               \
	};                                                                \
                                                                          \
	static file_operations _fops_##name = {                           \
		.release = _release_##name,                               \
		.read = _read_##name,                                     \
		.llseek = _llseek_##name,                                 \
	};                                                                \
                                                                          \
	static file *_open_root_##name(super_block *sb)                   \
	{                                                                 \
		inode *node = zalloc(sizeof(*node));                      \
		node->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;     \
		node->i_op = &_iops_##name;                               \
		node->i_private = vm_alloc(1);                            \
		fill_func(node->i_private, PAGE_SIZE);                    \
		node->i_size = strlen((char *)node->i_private);           \
		file *fp = zalloc(sizeof(*fp));                           \
		fp->f_inode = node;                                       \
		fp->f_count = 1;                                          \
		fp->f_fop = &_fops_##name;                                \
		return fp;                                                \
	}                                                                 \
                                                                          \
	static super_operations _sops_##name = {                          \
		.open_root = _open_root_##name,                           \
	};                                                                \
                                                                          \
	static void _proc_register_##name(super_block *proc_sb)           \
	{                                                                 \
		vfs_mount(proc_sb, "/" #name, sget(&_sops_##name));       \
	}                                                                 \
                                                                          \
	PROC_INIT(_proc_register_##name)

#endif /* _PROC_ENTRIES_GENERIC_H */
