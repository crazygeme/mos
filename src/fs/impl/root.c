#include <mm/mm.h>
#include <mm/mmap.h>
#include <device/blockdev.h>
#include <fs/cache.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <device/blockdev.h>
#include <device/blockdev.h>
#include <stddef.h>
#include <macro.h>
#include <config.h>
#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_dir.h>

unsigned fs_read_size = 0;
unsigned fs_write_size = 0;

/* =========================================================================
 * ext4 file / directory operations
 * ====================================================================== */

static void root_lock_lock(void);
static void root_lock_unlock(void);
static rmutex_t root_lock_;

typedef struct {
	ext4_file handle;
	struct ext4_fs *fs;
	file *owner;
	list_entry link;
	int orphan;
	file file;
	inode inode;
} ext4_open_file;

/* Independent opens retain one entry each; mappings and passed descriptors
 * retain their file object through f_count. Protected by the mount lock. */
static list_entry ext4_open_files;

static int ext4_refresh_size(file *fp)
{
	ext4_file *handle = fp->f_inode->i_private;
	struct stat st;
	int ret = ext4_fstat(handle, &st);

	if (!ret) {
		handle->fsize = st.st_size;
		fp->f_inode->i_size = st.st_size;
	}
	return ret ? -ret : 0;
}

static void ext4_touch_file(file *fp, int write)
{
	ext4_open_file *open = fp->f_inode->i_private;
	struct ext4_inode_ref ref;
	unsigned now = time_wall_sec();

	root_lock_lock();
	if (open->fs && !open->fs->read_only &&
	    ext4_fs_get_inode_ref(open->fs, open->handle.inode, &ref) == EOK) {
		if (write) {
			ext4_inode_set_modif_time(ref.inode, now);
			ext4_inode_set_change_inode_time(ref.inode, now);
		} else {
			ext4_inode_set_access_time(ref.inode, now);
		}
		ref.dirty = true;
		ext4_fs_put_inode_ref(&ref);
	}
	root_lock_unlock();
}

static int ext4_file_release(file *fp)
{
	ext4_open_file *open = fp->f_inode->i_private;
	ext4_file *f = &open->handle;
	struct ext4_inode_ref ref;
	list_entry *entry;
	int ret = EOK, remaining = 0;

	root_lock_lock();
	list_remove_entry(&open->link);
	for (entry = ext4_open_files.next;
	     open->orphan && entry != &ext4_open_files; entry = entry->next) {
		ext4_open_file *other =
			container_of(entry, ext4_open_file, link);
		if (other->fs == open->fs && other->handle.inode == f->inode) {
			remaining = 1;
			break;
		}
	}
	if (open->orphan && !remaining && open->fs) {
		ret = ext4_fs_get_inode_ref(open->fs, f->inode, &ref);
		if (ret == EOK) {
			int orphan = !ext4_inode_get_links_cnt(ref.inode);
			ext4_fs_put_inode_ref(&ref);
			if (orphan) {
				/* Cache reads acquire the mount lock after the cache lock. */
				root_lock_unlock();
				fs_page_cache_invalidate(fp);
				root_lock_lock();
				ret = ext4_fs_get_inode_ref(open->fs, f->inode,
							    &ref);
				if (ret == EOK) {
					ret = ext4_fs_truncate_inode(&ref, 0);
					if (ret == EOK) {
						ext4_inode_set_del_time(
							ref.inode,
							time_wall_sec());
						ref.dirty = true;
						ret = ext4_fs_free_inode(&ref);
					}
					ext4_fs_put_inode_ref(&ref);
				}
			}
		}
	}
	root_lock_unlock();
	if (ret)
		klog("ext4: inode %u release failed: %d\n", f->inode, ret);
	ext4_fclose(f);
	free(open);
	return ret ? -ret : 0;
}

static ssize_t ext4_file_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	ext4_file *f = fp->f_inode->i_private;
	ssize_t rcnt = ext4_refresh_size(fp);
	if (!rcnt)
		rcnt = fs_page_cache_read(fp, buf, size, pos);

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
	ext4_touch_file(fp, 0);
	return rcnt;
}

struct ext4_write_buffer {
	struct ext4_write_buffer *next;
	size_t size;
	char bytes[];
};

static int ext4_write_copy(const void *input, void *output, size_t size)
{
	mm_struct *mm = current->memory;
	vaddr_t cursor = (vaddr_t)input, end;
	LOCK_GUARD(&mm->mapping_lock);
	if (cursor >= mm->task_size || size > mm->task_size - cursor)
		return -EFAULT;
	end = cursor + size;
	while (cursor < end) {
		vm_region *region = vm_find_map_cached(current, cursor);
		if (!region || !(region->prot & PROT_READ))
			return -EFAULT;
		cursor = region->end < end ? region->end : end;
	}
	return ps_read_process_memory(current, input, output, size);
}

static void ext4_write_finish(struct ext4_write_buffer **snapshot)
{
	struct ext4_write_buffer *block = *snapshot;
	while (block) {
		struct ext4_write_buffer *next = block->next;
		free(block);
		block = next;
	}
	__sync_fetch_and_sub(&current->sched->vm_lock_depth, 1);
}

static ssize_t ext4_file_write(file *fp, const void *buf, size_t size,
			       loff_t *pos)
{
	ext4_file *f = fp->f_inode->i_private;
	size_t wcnt = 0;
	loff_t write_pos = *pos;
	int ret;
	struct ext4_write_buffer *snapshot
		__attribute__((cleanup(ext4_write_finish))) = NULL;

	/* Retain transfer storage until filesystem I/O and cleanup complete. */
	__sync_fetch_and_add(&current->sched->vm_lock_depth, 1);
	if (size && current->life->type == ps_user &&
	    (uintptr_t)buf < current->memory->task_size) {
		struct ext4_write_buffer **tail = &snapshot;
		size_t copied = 0;
		while (copied < size) {
			size_t chunk = size - copied;
			if (chunk > 64 * 1024)
				chunk = 64 * 1024;
			struct ext4_write_buffer *block =
				malloc(sizeof(*block) + chunk);
			if (!block)
				return -ENOMEM;
			block->next = NULL;
			block->size = chunk;
			*tail = block;
			tail = &block->next;
			if (ext4_write_copy((const char *)buf + copied,
					    block->bytes, chunk) < 0)
				return -EFAULT;
			copied += chunk;
		}
	}
	LOCK_GUARD(&root_lock_);

	ret = ext4_refresh_size(fp);
	if (ret)
		return ret;
	if (fp->f_inode)
		fs_page_cache_invalidate(fp);
	if (fp->f_flag & O_APPEND)
		write_pos = (loff_t)f->fsize;
	if ((uint64_t)write_pos > f->fsize) {
		ret = ext4_fenlarge(f, (uint64_t)write_pos);
		if (ret != EOK)
			return -ret;
	}
	if ((loff_t)ext4_ftell(f) != write_pos)
		ext4_fseek(f, write_pos, SEEK_SET);
	if (snapshot) {
		for (struct ext4_write_buffer *block = snapshot; block;
		     block = block->next) {
			size_t count = 0;
			ret = ext4_fwrite(f, block->bytes, block->size, &count);
			wcnt += count;
			if (ret != EOK || count < block->size)
				break;
		}
	} else {
		ret = ext4_fwrite(f, buf, size, &wcnt);
	}
	fs_write_size += wcnt;
	if (ret != EOK)
		return -ret;
	*pos = write_pos + (loff_t)wcnt;
	if (fp->f_inode)
		fp->f_inode->i_size = f->fsize;
	ext4_touch_file(fp, 1);
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
		if (ext4_refresh_size(fp))
			return -EIO;
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

	if (sb)
		path = sb->s_mountpoint;
	else if (fp->f_name)
		path = fp->f_name;

	if (!path)
		return 0;

	if (CURRENT_TASK() && current->fs)
		vm_flush_file_dirty(current->memory, fp);

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

	blockdev_flush_all();
	return 0;
}

static int ext4_file_getattr(file *fp, struct stat *s)
{
	ext4_file *f = fp->f_inode->i_private;
	return ext4_fstat(f, s);
}

static int ext4_file_readlink(file *fp, char *buf, size_t size, size_t *length)
{
	ext4_file handle = *(ext4_file *)fp->f_inode->i_private;
	int ret;
	if (!S_ISLNK(fp->f_inode->i_mode))
		return -EINVAL;
	handle.flags = O_RDONLY;
	handle.fpos = 0;
	ret = ext4_fread(&handle, buf, size, length);
	return ret ? -ret : 0;
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

static int ext4_file_read_page(file *fp, uint64_t offset, void *buf)
{
	/* Page I/O owns its cursor independently of other faults and descriptor I/O. */
	ext4_file page = *(ext4_file *)fp->f_inode->i_private;
	size_t rcnt = 0;
	int ret;

	if (ext4_fseek(&page, offset, SEEK_SET) != EOK)
		return -EIO;
	ret = ext4_fread(&page, buf, PAGE_SIZE, &rcnt);
	if (rcnt < PAGE_SIZE)
		memset((char *)buf + rcnt, 0, PAGE_SIZE - rcnt);
	if (ret != EOK)
		return -EIO;
	return 0;
}

static int ext4_file_write_page(file *fp, uint64_t offset, const void *buf)
{
	ext4_file page = *(ext4_file *)fp->f_inode->i_private;
	size_t wcnt = 0;
	int ret;

	fs_page_cache_invalidate(fp);
	if (ext4_fseek(&page, offset, SEEK_SET) != EOK)
		return -EIO;
	ret = ext4_fwrite(&page, buf, PAGE_SIZE, &wcnt);
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
	ext4_touch_file(fp, 1);
	return 0;
}

static const file_operations ext4_file_fops = {
	.release = ext4_file_release,
	.getattr = ext4_file_getattr,
	.readlink = ext4_file_readlink,
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
	struct stat state;
	unsigned out = 0;
	int ret = ext4_fstat(&dir->f, &state);
	if (ret != EOK)
		return -ret;
	uint64_t end = state.st_size;
	if ((uint64_t)*pos >= end)
		return 0;
	/* Directory offsets are opaque backing-store cookies. */
	dir->next_off = *pos;
	while (count > out) {
		uint64_t cookie = dir->next_off;
		const ext4_direntry *entry = ext4_dir_entry_next(dir);
		if (!entry) {
			*pos = end;
			break;
		}
		if (!entry->inode || !entry->name_length ||
		    entry->inode_type == EXT4_DIRENTRY_DIR_CSUM) {
			*pos = dir->next_off == ~0ULL ? end : dir->next_off;
			continue;
		}
		unsigned size =
			ROUND_UP(NAME_OFFSET() + entry->name_length + 1);
		if (size > count - out) {
			dir->next_off = cookie;
			*pos = cookie;
			break;
		}
		struct linux_dirent *record = (void *)((char *)buf + out);
		memset(record, 0, size);
		record->d_ino = entry->inode;
		record->d_reclen = size;
		/* Export the backing-store end offset, not the iterator sentinel. */
		record->d_off = dir->next_off == ~0ULL ? end : dir->next_off;
		memcpy(record->d_name, entry->name, entry->name_length);
		*pos = record->d_off;
		out += size;
	}
	return out;
}

static loff_t ext4_dir_llseek(file *fp, loff_t offset, int whence)
{
	ext4_dir *dir = fp->f_inode->i_private;
	if (whence != SEEK_SET)
		return -EACCES;
	if (offset < 0 || (uint64_t)offset > 0xffffffffULL)
		return -EINVAL;
	dir->next_off = offset;
	fp->f_pos = offset;
	return offset;
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
	ext4_open_file *open = zalloc(sizeof(*open));
	if (!open)
		return NULL;
	open->handle = *(ext4_file *)content;
	inode *node = &open->inode;
	file *fp = &open->file;
	node->i_private = open;
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

static file *ext4_path_open(const char *path, int flag, char **target_path)
{
	ext4_file handle;
	struct ext4_inline_link inline_link;
	ext4_file *f = NULL;
	ext4_dir *dir = NULL;
	unsigned uid = current->credentials->uid;
	unsigned gid = current->credentials->gid;
	char *pre_res = NULL; /* buffer for intermediate symlink resolution */
	char *resolved =
		NULL; /* absolute path of the current final component */
	char *link_target = NULL; /* target text, separate from cur_path */
	const char *cur_path = path;
	size_t link_len;
	int ret, check;
	int depth = 0;
	file *fp = NULL;
	struct stat s;

	/*
	 * First try the direct lwext4 operation.  The expensive prefix walk is
	 * only needed when an intermediate component is a symlink; ordinary
	 * opens should not pay one ext4_fopen2/ext4_fstat pair per component.
	 */
retry_open:

	/* ---- Case 1: trailing '/' means "open as dir" ---- */
	if (cur_path[strlen(cur_path) - 1] == '/') {
		dir = zalloc(sizeof(*dir));
		ret = ext4_dir_open(dir, cur_path);
		if (ret != EOK) {
			free(dir);
			dir = NULL;
			goto resolve_prefix;
		}
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
	f = &handle;
	memset(f, 0, sizeof(*f));
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
	ret = ext4_fopen2_stat(f, cur_path, flag, &s,
			       target_path ? &inline_link : NULL);
	if (check != EOK && ret == EOK) {
		ext4_fchown(f, uid, gid);
		ext4_file_set_ctime(cur_path, time_wall_sec());
	}

	if (ret != EOK)
		goto resolve_prefix;

	/* O_NOFOLLOW: return the symlink itself without following it. */
	if (S_ISLNK(s.st_mode) && (flag & O_NOFOLLOW)) {
		if (target_path) {
			char *text = name_get();
			ext4_file link_handle = *f;
			if (!text)
				goto fail;
			link_handle.flags = O_RDONLY;
			link_handle.fpos = 0;
			if (inline_link.length) {
				link_len = inline_link.length;
				memcpy(text, inline_link.target, link_len);
				ret = EOK;
			} else {
				ret = ext4_fread(&link_handle, text,
						 MAX_PATH - 1, &link_len);
			}
			if (ret || !link_len || link_len >= MAX_PATH) {
				name_put(text);
				goto fail;
			}
			text[link_len] = 0;
			if (fs_resolve_symlink_path(cur_path, text, link_len)) {
				name_put(text);
				goto fail;
			}
			*target_path = text;
			goto done;
		}
		fp = ext4_alloc_file(f);
		if (!fp)
			goto fail;
		fp->f_inode->i_mode = s.st_mode;
		fp->f_inode->i_ino = s.st_ino;
		fp->f_inode->i_size = s.st_size;
		fp->f_name = strdup(path); /* original (pre-resolution) path */
		goto done;
	}

	/* Allocate a resolution buffer only when we actually encounter a symlink */
	if (S_ISLNK(s.st_mode)) {
		resolved = name_get();
		link_target = name_get();
		if (!resolved || !link_target)
			goto fail;
	}

	while (S_ISLNK(s.st_mode)) {
		/* Guard against symlink loops */
		if (++depth > MAX_SYMLINK_DEPTH)
			goto fail;

		/* Preserve cur_path and read the open link independently of access mode. */
		ext4_file link_handle = *f;
		link_handle.flags = O_RDONLY;
		link_handle.fpos = 0;
		ret = ext4_fread(&link_handle, link_target, MAX_PATH - 1,
				 &link_len);
		ext4_fclose(f);
		if (ret != EOK)
			goto fail;
		link_target[link_len] = '\0';

		if (fs_resolve_symlink_path(cur_path, link_target, link_len) !=
		    0)
			goto fail;

		strcpy(resolved, link_target);
		cur_path = resolved;

		ret = ext4_fopen2_stat(f, cur_path, flag, &s, NULL);
		if (ret != EOK)
			goto fail;
	}

	/* ---- Case 3: final target is a directory ---- */
	if (S_ISDIR(s.st_mode)) {
		ext4_fclose(f);
		f = NULL;

		dir = zalloc(sizeof(*dir));
		ret = ext4_dir_open(dir, cur_path);
		if (ret != EOK)
			goto fail;
		ext4_dir_entry_rewind(dir);
		fp = ext4_alloc_dir(dir);
	} else {
		fp = ext4_alloc_file(f);
		if (!fp)
			goto fail;
	}

	fp->f_inode->i_mode = s.st_mode;
	fp->f_inode->i_ino = s.st_ino;
	fp->f_inode->i_size = s.st_size;
	fp->f_name = strdup(cur_path);
	goto done;

resolve_prefix:
	if (f) {
		if (f->mp)
			ext4_fclose(f);
		f = NULL;
	}
	if (pre_res)
		goto fail;

	/*
	 * lwext4 does not follow symlinks in non-final components; without
	 * this fallback, paths like "/var/mail/foo" where "mail" is a symlink
	 * return ENOENT from lwext4.
	 */
	pre_res = name_get();
	if (!pre_res || ext4_resolve_prefix(path, pre_res) != 0 ||
	    strcmp(pre_res, path) == 0)
		goto fail;
	cur_path = pre_res;
	goto retry_open;

fail:
	fp = NULL;
	if (dir)
		free(dir);
done:
	if (f && f->mp)
		ext4_fclose(f);
	if (pre_res)
		name_put(pre_res);
	if (resolved)
		name_put(resolved);
	if (link_target)
		name_put(link_target);
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

static file *ext4_open_link(super_block *sb, const char *path, int flag,
			    char **target)
{
	ext4_mount_info *mi = sb->s_fs_info;
	char *storage = NULL;
	const char *full = path;

	/* Root paths already have the form expected by lwext4. Only secondary
	 * mounts need a temporary buffer to prepend their lwext4 mount name. */
	if (strcmp(mi->mp, "/") || path[0] != '/') {
		storage = name_get();
		if (!storage)
			return NULL;
		sprintf(storage, "%s%s", mi->mp,
			path[0] == '/' ? path + 1 : path);
		full = storage;
	}
	root_lock_lock();
	file *ret = ext4_path_open(full, flag, target);
	if (ret && ret->f_fop == &ext4_file_fops) {
		ext4_open_file *open = ret->f_inode->i_private;
		struct ext4_sblock *disk_sb;
		if (ext4_get_sblock(mi->mp, &disk_sb) == EOK)
			open->fs = container_of(disk_sb, struct ext4_fs, sb);
		open->owner = ret;
		list_init(&open->link);
		list_insert_tail(&ext4_open_files, &open->link);
	}
	if (ret && ret->f_inode) {
		ret->f_inode->i_pgcache_tag = sb;
		if (flag & O_TRUNC)
			fs_page_cache_invalidate(ret);
	}
	root_lock_unlock();
	if (storage)
		name_put(storage);
	return ret;
}

static file *ext4_open(super_block *sb, const char *path, int flag)
{
	return ext4_open_link(sb, path, flag, NULL);
}

static file *ext4_open_root(super_block *sb, int flag)
{
	ext4_mount_info *mi = sb->s_fs_info;
	file *ret = ext4_path_open(mi->mp, O_RDONLY, NULL);

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
		blockdev_detach_file(mi->loop_name);
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
	struct rb_node *node;
	int ret;

	if (!sb)
		return 0;

	ret = fs_sync_super_one(sb);
	if (ret)
		return ret;

	mutex_lock(&cur->s_lock);
	for (node = rb_first(&cur->s_mounts); node; node = rb_next(node)) {
		vfs_mount_node *mount = rb_entry(node, vfs_mount_node, rb_node);
		super_block *child = mount->sb;

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
	ext4_dir *dir = zalloc(sizeof(*dir));
	const ext4_direntry *entry;
	int ret;

	if (!dir)
		return ENOMEM;
	ret = ext4_dir_open(dir, full);
	if (ret != EOK) {
		free(dir);
		return ret;
	}

	ext4_dir_entry_rewind(dir);
	while ((entry = ext4_dir_entry_next(dir)) != NULL) {
		if (entry->inode == 0 || entry->name_length == 0 ||
		    entry->inode_type == EXT4_DIRENTRY_DIR_CSUM)
			continue;
		if (entry->name_length == 1 && entry->name[0] == '.')
			continue;
		if (entry->name_length == 2 && entry->name[0] == '.' &&
		    entry->name[1] == '.')
			continue;
		ext4_dir_close(dir);
		free(dir);
		return ENOTEMPTY;
	}

	ext4_dir_close(dir);
	free(dir);
	return EOK;
}

static int ext4_parent_dir_check(const char *full)
{
	char *parent = name_get();
	char *slash;
	ext4_dir *dir = zalloc(sizeof(*dir));
	int ret;

	if (!dir || !parent) {
		free(dir);
		if (parent)
			name_put(parent);
		return ENOMEM;
	}
	strcpy(parent, full);
	slash = strrchr(parent, '/');
	if (!slash) {
		name_put(parent);
		free(dir);
		return ENOENT;
	}

	if (slash == parent)
		parent[1] = '\0';
	else
		*slash = '\0';

	ret = ext4_dir_open(dir, parent);
	if (ret == EOK)
		ext4_dir_close(dir);

	name_put(parent);
	free(dir);
	return ret;
}

static int ext4_mkdir(super_block *sb, const char *path, unsigned mode)
{
	char *full = name_get();
	unsigned uid = current->credentials->uid;
	unsigned gid = current->credentials->gid;
	int ret;
	ext4_dir *dir = zalloc(sizeof(*dir));
	if (!dir || !full) {
		free(dir);
		if (full)
			name_put(full);
		return -ENOMEM;
	}
	ext4_full_path(sb, path, full);
	ret = ext4_parent_dir_check(full);
	if (ret == EOK)
		ret = ext4_dir_mk(full);
	if (ret == EOK) {
		uint32_t t = (uint32_t)time_wall_sec();
		ext4_file_set_mtime(full, t);
		ext4_file_set_ctime(full, t);
		ext4_chown(full, uid, gid);
		if (ext4_dir_open(dir, full) == EOK) {
			ext4_fchmod(&dir->f, S_IFDIR | (mode & 0777));
			ext4_dir_close(dir);
		}
	}
	name_put(full);
	free(dir);
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
	char *full = name_get(), *parent_path = name_get();
	ext4_file child_file, parent_file;
	struct ext4_sblock *disk_sb;
	struct ext4_fs *fs;
	struct ext4_inode_ref child, parent;
	char *slash;
	list_entry *entry;
	int ret, retained = 0;

	if (!full || !parent_path) {
		ret = ENOMEM;
		goto done;
	}
	ext4_full_path(sb, path, full);
	root_lock_lock();
	ret = ext4_get_sblock(full, &disk_sb);
	if (ret)
		goto unlock;
	fs = container_of(disk_sb, struct ext4_fs, sb);
	if (fs->read_only) {
		ret = EROFS;
		goto unlock;
	}
	ret = ext4_fopen2(&child_file, full, O_RDONLY);
	if (ret)
		goto unlock;
	for (entry = ext4_open_files.next; entry != &ext4_open_files;
	     entry = entry->next) {
		ext4_open_file *open =
			container_of(entry, ext4_open_file, link);
		if (open->fs == fs && open->handle.inode == child_file.inode) {
			retained = 1;
			break;
		}
	}
	if (!retained) {
		ret = ext4_fremove(full);
		goto close_child;
	}
	strcpy(parent_path, full);
	slash = strrchr(parent_path, '/');
	if (slash == parent_path)
		parent_path[1] = '\0';
	else
		*slash = '\0';
	ret = ext4_fopen2(&parent_file, parent_path, O_RDONLY);
	if (ret)
		goto close_child;
	ret = ext4_fs_get_inode_ref(fs, parent_file.inode, &parent);
	if (ret)
		goto close_parent;
	ret = ext4_fs_get_inode_ref(fs, child_file.inode, &child);
	if (ret)
		goto put_parent;
	if (ext4_inode_is_type(&fs->sb, child.inode,
			       EXT4_INODE_MODE_DIRECTORY)) {
		ret = EISDIR;
		goto put_child;
	}
	slash = strrchr(full, '/');
	ret = ext4_dir_remove_entry(&parent, slash + 1, strlen(slash + 1));
	if (!ret) {
		unsigned now = time_wall_sec();
		ext4_fs_inode_links_count_dec(&child);
		ext4_inode_set_change_inode_time(child.inode, now);
		ext4_inode_set_change_inode_time(parent.inode, now);
		ext4_inode_set_modif_time(parent.inode, now);
		child.dirty = parent.dirty = true;
		for (entry = ext4_open_files.next; entry != &ext4_open_files;
		     entry = entry->next) {
			ext4_open_file *open =
				container_of(entry, ext4_open_file, link);
			file *fp = open->owner;
			if (open->fs == fs &&
			    open->handle.inode == child_file.inode &&
			    !ext4_inode_get_links_cnt(child.inode))
				open->orphan = 1;
			if (open->fs == fs &&
			    open->handle.inode == child_file.inode &&
			    fp->f_name &&
			    (!ext4_inode_get_links_cnt(child.inode) ||
			     !strcmp(fp->f_name, full))) {
				free(fp->f_name);
				fp->f_name = NULL;
			}
		}
	}
put_child:
	ext4_fs_put_inode_ref(&child);
put_parent:
	ext4_fs_put_inode_ref(&parent);
close_parent:
	ext4_fclose(&parent_file);
close_child:
	ext4_fclose(&child_file);
unlock:
	root_lock_unlock();
done:
	if (full)
		name_put(full);
	if (parent_path)
		name_put(parent_path);
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
	unsigned uid = current->credentials->uid;
	unsigned gid = current->credentials->gid;
	int ret;
	ext4_full_path(sb, oldpath, full1);
	ext4_full_path(sb, newpath, full2);
	ret = ext4_flink(full1, full2);
	if (ret == EOK) {
		ext4_file_set_ctime(full1, (uint32_t)time_wall_sec());
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
	unsigned uid = current->credentials->uid;
	unsigned gid = current->credentials->gid;
	int ret;
	ext4_full_path(sb, linkpath, full);
	ret = ext4_fsymlink(target, full);
	if (ret == EOK) {
		uint32_t t = (uint32_t)time_wall_sec();
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
	unsigned uid = current->credentials->uid;
	unsigned gid = current->credentials->gid;
	int ret;
	ext4_full_path(sb, oldpath, full1);
	ext4_full_path(sb, newpath, full2);
	if (ext4_path_is_descendant(full1, full2)) {
		name_put(full1);
		name_put(full2);
		return -EINVAL;
	}
	root_lock_lock();
	ext4_file source, target;
	if (ext4_fopen2(&source, full1, O_RDONLY) == EOK) {
		if (ext4_fopen2(&target, full2, O_RDONLY) == EOK) {
			int same = source.mp == target.mp &&
				   source.inode == target.inode;
			ext4_fclose(&target);
			ext4_fclose(&source);
			if (same) {
				root_lock_unlock();
				name_put(full1);
				name_put(full2);
				return 0;
			}
		} else {
			ext4_fclose(&source);
		}
	}
	ret = ext4_frename(full1, full2);
	if (ret == EEXIST) {
		/* POSIX rename(2) must replace the destination if it exists */
		ret = ext4_unlink(sb, newpath);
		if (!ret)
			ret = ext4_frename(full1, full2);
		else
			ret = -ret;
	}
	if (ret == EOK) {
		list_entry *entry;
		for (entry = ext4_open_files.next; entry != &ext4_open_files;
		     entry = entry->next) {
			ext4_open_file *open =
				container_of(entry, ext4_open_file, link);
			file *fp = open->owner;
			if (fp->f_name && !strcmp(fp->f_name, full1)) {
				free(fp->f_name);
				fp->f_name = strdup(full2);
			}
		}
		uint32_t t = (uint32_t)time_wall_sec();
		ext4_file_set_mtime(full2, t);
		ext4_file_set_ctime(full2, t);
		ext4_chown(full2, uid, gid);
	}
	root_lock_unlock();
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
	if (ret == ENOENT) {
		char *resolved = name_get();

		/* Resolve directory symlinks while preserving the final link. */
		if (resolved && ext4_resolve_prefix(full, resolved) == 0 &&
		    strcmp(full, resolved) != 0) {
			strcpy(full, resolved);
			ret = ext4_readlink(full, buf, bufsiz, rcnt);
		}
		if (resolved)
			name_put(resolved);
	}
	if (ret == ENOENT) {
		ext4_file f;
		struct stat st;

		/* lwext4 reports ENOENT for both absent paths and type mismatches.
		 * Linux readlink requires EINVAL for an existing non-symlink. */
		if (ext4_fopen2(&f, full, O_RDONLY) == EOK) {
			int stat_ret = ext4_fstat(&f, &st);
			ext4_fclose(&f);
			if (stat_ret == EOK && !S_ISLNK(st.st_mode))
				ret = EINVAL;
		}
	}
	name_put(full);
	return ret ? -ret : 0;
}

static int ext4_statfs_op(super_block *sb, struct statfs64 *buf)
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
	.open_link = ext4_open_link,
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
/* Filesystem-specific adapter: storage drivers never call lwext4. */
#define EXT4_BLOCK_ADAPTERS 32
struct ext4_block_adapter {
	char name[32];
	struct ext4_blockdev device;
	struct ext4_blockdev_iface iface;
	blockdev_handle *handle;
};
static struct ext4_block_adapter ext4_adapters[EXT4_BLOCK_ADAPTERS];

static int ext4_block_open(struct ext4_blockdev *device)
{
	struct ext4_block_adapter *adapter = device->aux;
	adapter->handle = blockdev_open(adapter->name);
	if (!adapter->handle) return ENODEV;
	unsigned size = blockdev_sector_size(adapter->handle);
	if (size != adapter->iface.ph_bsize) {
		uint8_t *buffer = malloc(size);
		if (!buffer) { blockdev_close(adapter->handle); adapter->handle = NULL; return ENOMEM; }
		free(adapter->iface.ph_bbuf);
		adapter->iface.ph_bbuf = buffer;
		adapter->iface.ph_bsize = size;
	}
	adapter->iface.ph_bcnt = blockdev_sector_count(adapter->handle);
	device->part_size = adapter->iface.ph_bcnt * adapter->iface.ph_bsize;
	return EOK;
}

static int ext4_block_close(struct ext4_blockdev *device)
{
	struct ext4_block_adapter *adapter = device->aux;
	blockdev_close(adapter->handle);
	adapter->handle = NULL;
	return EOK;
}

static int ext4_block_read(struct ext4_blockdev *device, void *buffer, uint64_t sector, uint32_t count)
{
	struct ext4_block_adapter *adapter = device->aux;
	int result = blockdev_read(adapter->handle, buffer, sector, count);
	return result < 0 ? -result : result;
}

static int ext4_block_write(struct ext4_blockdev *device, const void *buffer, uint64_t sector, uint32_t count)
{
	struct ext4_block_adapter *adapter = device->aux;
	int result = blockdev_write(adapter->handle, buffer, sector, count);
	return result < 0 ? -result : result;
}

static int ext4_prepare_block_device(const char *name)
{
	struct ext4_block_adapter *adapter = NULL;
	blockdev_handle *handle;
	unsigned size;
	int result = EOK;
	root_lock_lock();
	for (unsigned i = 0; i < EXT4_BLOCK_ADAPTERS; i++) {
		if (!strcmp(ext4_adapters[i].name, name)) {
			root_lock_unlock();
			return EOK;
		}
		if (!adapter && !ext4_adapters[i].name[0]) adapter = &ext4_adapters[i];
	}
	if (!adapter) { root_lock_unlock(); return ENOSPC; }
	handle = blockdev_open(name);
	if (!handle) { root_lock_unlock(); return ENODEV; }
	size = blockdev_sector_size(handle);
	adapter->iface.ph_bbuf = malloc(size);
	if (!adapter->iface.ph_bbuf) result = ENOMEM;
	else {
		strncpy(adapter->name, name, sizeof(adapter->name) - 1);
		adapter->iface.ph_bsize = size;
		adapter->iface.ph_bcnt = blockdev_sector_count(handle);
		adapter->iface.open = ext4_block_open;
		adapter->iface.close = ext4_block_close;
		adapter->iface.bread = ext4_block_read;
		adapter->iface.bwrite = ext4_block_write;
		adapter->device.bdif = &adapter->iface;
		adapter->device.part_size = adapter->iface.ph_bcnt * size;
		adapter->device.aux = adapter;
		result = ext4_device_register(&adapter->device, NULL, name);
		if (result) {
			free(adapter->iface.ph_bbuf);
			memset(adapter, 0, sizeof(*adapter));
		}
	}
	blockdev_close(handle);
	root_lock_unlock();
	return result;
}

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
		const char *ln = blockdev_attach_file(dev);
		if (!ln) {
			name_put(mp);
			return NULL;
		}
		strncpy(loop_auto, ln, sizeof(loop_auto) - 1);
		if (!blockdev_lookup_mountable(loop_auto, &bdev)) {
			blockdev_detach_file(loop_auto);
			name_put(mp);
			return NULL;
		}
		dev_name = bdev.name;
	}
	/* lwext4 requires the mount point to end with '/' */
	strncpy(mp, target, MAX_PATH - 2);
	mp[MAX_PATH - 2] = '\0';
	n = strlen(mp);
	if (n > 0 && mp[n - 1] != '/') {
		mp[n] = '/';
		mp[n + 1] = '\0';
	}

	read_only = (flags & MS_RDONLY) != 0;

	ret = ext4_prepare_block_device(dev_name);
	if (ret == EOK)
		ret = ext4_mount(dev_name, mp, read_only);
	if (ret != EOK) {
		if (loop_auto[0])
			blockdev_detach_file(loop_auto);
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
static void root_lock_lock(void)
{
	vm_lock_enter(&root_lock_);
}

static void root_lock_unlock(void)
{
	vm_lock_leave(&root_lock_);
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
	cur->fs->root = ext4_get(devname);
	if (ext4_prepare_block_device(devname) != EOK || ext4_mount(devname, "/", true) != EOK) {
		printk("ext4: cannot mount root block device %s\n", devname);
		return;
	} /* read-only until init remounts rw */
	ext4_mount_setup_locks("/", &root_lock);

	/* Populate root sb metadata for /proc/mounts. */
	sprintf(cur->fs->root->s_devname, "/dev/%s", devname);
	strncpy(cur->fs->root->s_fstype, "ext3",
		sizeof(cur->fs->root->s_fstype) - 1);
	strncpy(cur->fs->root->s_mountpoint, "/",
		sizeof(cur->fs->root->s_mountpoint) - 1);
	cur->fs->root->s_flags = MS_RDONLY;
}

static void ext_fs_type_init()
{
	printk("mnt: registered ext3 file type\n");
	fs_register_type(&ext3_fs_type);

	printk("mnt: registered ext4 file type\n");
	fs_register_type(&ext4_fs_type);

	printk("mnt: registered vfat file type\n");
	fs_register_type(&vfat_fs_type);
	vm_lock_init(&root_lock_);
	list_init(&ext4_open_files);
}

KERNEL_INIT(2, ext_fs_type_init);
KERNEL_INIT(3, fs_mount_root);
