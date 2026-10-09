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
 * The provider registers its entry during general kernel initialization.
 *
 * Usage:
 *   static void fill(void *buf, size_t size) { sprintf(buf, "hello\n"); }
 *   DEFINE_PROC_FILE(hello, fill);
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <proc/proc.h>
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

/* Entries owns snapshots, metadata, seeking, and per-open buffer lifetime. */
#define _DEFINE_PROC_SNAPSHOT(name, fill_func)                  \
	static char *_snapshot_##name(void *data, unsigned tag, \
				      unsigned *length)         \
	{                                                       \
		proc_buf_t *pb = proc_buf_new();                \
		char *buffer;                                   \
		(void)data;                                     \
		(void)tag;                                      \
		if (!pb)                                        \
			return NULL;                            \
		fill_func(pb);                                  \
		buffer = pb->buf;                               \
		*length = pb->len;                              \
		free(pb);                                       \
		return buffer;                                  \
	}                                                       \
	static const vfs_entry_attribute_ops _ops_##name = {    \
		.snapshot = _snapshot_##name,                   \
	}

#define DEFINE_PROC_FILE(name, fill_func)                          \
	_DEFINE_PROC_SNAPSHOT(name, fill_func);                    \
	static void _proc_register_##name(void)                    \
	{                                                          \
		vfs_entry_attribute(procfs_entries(), #name, 0444, \
				    &_ops_##name, NULL, 0);        \
	}                                                          \
	KERNEL_INIT(4, _proc_register_##name)

#define DEFINE_PROC_FILE_AT(mount_path, name, fill_func)                      \
	_DEFINE_PROC_SNAPSHOT(name, fill_func);                               \
	static void _proc_register_##name(void)                               \
	{                                                                     \
		vfs_entry_node *root = procfs_entries();                      \
		const char *path = mount_path;                                \
		char *copy = strdup(path), *leaf = copy;                      \
		if (!copy)                                                    \
			return;                                               \
		for (char *p = copy; *p; p++) {                               \
			if (*p != '/')                                        \
				continue;                                     \
			*p = 0;                                               \
			if (*leaf)                                            \
				root = vfs_entry_directory(root, leaf);       \
			leaf = p + 1;                                         \
		}                                                             \
		vfs_entry_attribute(root, leaf, 0444, &_ops_##name, NULL, 0); \
		free(copy);                                                   \
	}                                                                     \
	KERNEL_INIT(4, _proc_register_##name)

#endif
