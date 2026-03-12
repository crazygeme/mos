#ifndef _FS_MOUNT_DEBUG_GENERIC_H
#define _FS_MOUNT_DEBUG_GENERIC_H
#include <fs/fs.h>
#include <fs/mount.h>
#include <fs/mount/proc.h>
#include <hw/time.h>
#include <lib/klib.h>

#define DEFINE_DEBUG_FS_FILE(name, fill_func)                                 \
	static int _getattr(inode *inode, struct stat *s)                     \
	{                                                                     \
		s->st_atime = time_now_ms();                                  \
		s->st_mode = inode->i_mode;                                   \
		s->st_size = PAGE_SIZE;                                       \
		s->st_blksize = PAGE_SIZE;                                    \
		s->st_blocks = 0;                                             \
		s->st_ctime = time_now_ms();                                  \
		s->st_dev = 0;                                                \
		s->st_gid = 0;                                                \
		s->st_ino = 0;                                                \
		s->st_mtime = 0;                                              \
		s->st_uid = 0;                                                \
		return 0;                                                     \
	}                                                                     \
                                                                              \
	static int _release(file *file)                                       \
	{                                                                     \
		vm_free(file->f_inode->i_private, 1);                         \
		free(file->f_inode);                                          \
		free(file);                                                   \
	}                                                                     \
                                                                              \
	static ssize_t _read(file *file, void *buf, size_t size, loff_t *pos) \
	{                                                                     \
		loff_t offset = *pos;                                         \
		ssize_t left = PAGE_SIZE - offset;                            \
		ssize_t read_size = size > left ? left : size;                \
                                                                              \
		if (read_size <= 0)                                           \
			return 0;                                             \
                                                                              \
		memcpy(buf, (char *)file->f_inode->i_private + offset,        \
		       read_size);                                            \
		*pos = offset + read_size;                                    \
		return read_size;                                             \
	}                                                                     \
                                                                              \
	static inode_operations inode_op = {                                  \
		.getattr = _getattr,                                          \
	};                                                                    \
                                                                              \
	static file_operations file_op = {                                    \
		.release = _release,                                          \
		.read = _read,                                                \
	};                                                                    \
                                                                              \
	static file *_open_root(super_block *sb)                              \
	{                                                                     \
		inode *node = calloc(1, sizeof(*node));                       \
		node->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;         \
		node->i_op = &inode_op;                                       \
		node->i_private = vm_alloc(1);                                \
		fill_func(node->i_private, PAGE_SIZE);                        \
		file *fp = calloc(1, sizeof(*fp));                            \
		fp->f_inode = node;                                           \
		fp->f_count = 1;                                              \
		fp->f_fop = &file_op;                                         \
		return fp;                                                    \
	}                                                                     \
                                                                              \
	static super_operations operation = {                                 \
		.open_root = _open_root,                                      \
	};                                                                    \
                                                                              \
	static void debugfs_##name(super_block *proc)                         \
	{                                                                     \
		vfs_mount(proc, "/" #name, sget(&operation));                 \
	}                                                                     \
                                                                              \
	DEBUGFS_INIT(debugfs_##name);

#endif