/*
 * src/fs/tmpfs.c - in-memory temporary filesystem
 *
 * Used for /dev/shm (POSIX shared memory via shm_open + mmap).
 *
 * Files store data in lazily-allocated PAGE_SIZE chunks.
 * Directories hold a doubly-linked list of (name, node) dentries.
 * All state lives in kernel heap; nothing persists across reboots.
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/list.h>
#include <hw/time.h>
#include <macro.h>
#include <errno.h>
#include <unistd.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* ── Node / dirent structures ────────────────────────────────────────────── */

typedef struct _tmpfs_node tmpfs_node;
typedef struct _tmpfs_dirent tmpfs_dirent;

struct _tmpfs_dirent {
	char *name;
	tmpfs_node *node;
	list_entry list;
};

struct _tmpfs_node {
	uint32_t mode;
	uint64_t ino;
	unsigned size; /* file size in bytes */
	unsigned uid;
	unsigned gid;
	unsigned atime;
	unsigned mtime;
	unsigned ctime;
	/* Files: lazily-allocated PAGE_SIZE blocks */
	char **pages;
	unsigned n_pages;
	tmpfs_node *parent;
	/* Dirs: sentinel head of child dirent list */
	list_entry children;
	unsigned ref;
};

/* ── Per-superblock info ──────────────────────────────────────────────────── */

typedef struct {
	tmpfs_node *root;
	unsigned next_ino;
	spinlock_t lock;
} tmpfs_sb_info;

/* ── Node lifecycle ───────────────────────────────────────────────────────── */

static tmpfs_node *tmpfs_node_alloc(tmpfs_sb_info *sbi, uint32_t mode)
{
	tmpfs_node *n = zalloc(sizeof(*n));
	unsigned now = time_now_sec();
	int irq;

	spinlock_lock(&sbi->lock, &irq);
	n->ino = sbi->next_ino++;
	spinlock_unlock(&sbi->lock, irq);

	n->mode = mode;
	n->ref = 1;
	n->atime = n->mtime = n->ctime = now;
	list_init(&n->children);
	return n;
}

static void tmpfs_touch_mctime(tmpfs_node *n)
{
	unsigned now = time_now_sec();

	n->mtime = now;
	n->ctime = now;
}

static void tmpfs_touch_ctime(tmpfs_node *n)
{
	n->ctime = time_now_sec();
}

static void tmpfs_node_get(tmpfs_node *n)
{
	__sync_add_and_fetch(&n->ref, 1);
}

static void tmpfs_node_put(tmpfs_node *n)
{
	unsigned i;

	if (__sync_sub_and_fetch(&n->ref, 1) != 0)
		return;

	if (n->pages) {
		for (i = 0; i < n->n_pages; i++)
			free(n->pages[i]);
		free(n->pages);
	}
	free(n);
}

/* ── Directory helpers ────────────────────────────────────────────────────── */

static tmpfs_node *tmpfs_dir_lookup(tmpfs_node *dir, const char *name)
{
	list_entry *e;

	for (e = dir->children.next; e != &dir->children; e = e->next) {
		tmpfs_dirent *de = container_of(e, tmpfs_dirent, list);

		if (strcmp(de->name, name) == 0)
			return de->node;
	}
	return NULL;
}

static void tmpfs_dir_add(tmpfs_node *dir, const char *name, tmpfs_node *node)
{
	tmpfs_dirent *de = zalloc(sizeof(*de));

	de->name = strdup(name);
	de->node = node;
	node->parent = dir;
	tmpfs_node_get(node);
	list_insert_tail(&dir->children, &de->list);
	tmpfs_touch_mctime(dir);
}

static int tmpfs_dir_remove(tmpfs_node *dir, const char *name)
{
	list_entry *e;

	for (e = dir->children.next; e != &dir->children; e = e->next) {
		tmpfs_dirent *de = container_of(e, tmpfs_dirent, list);

		if (strcmp(de->name, name) == 0) {
			list_remove_entry(e);
			tmpfs_node_put(de->node);
			free(de->name);
			free(de);
			tmpfs_touch_mctime(dir);
			return 0;
		}
	}
	return -ENOENT;
}

/* ── Path walking ─────────────────────────────────────────────────────────── */

static tmpfs_node *tmpfs_walk(tmpfs_node *root, const char *path)
{
	const char *p = path;
	tmpfs_node *cur = root;
	char *component = malloc(256);

	if (!component)
		return NULL;

	while (*p == '/')
		p++;

	while (*p && cur) {
		unsigned len = 0;

		while (p[len] && p[len] != '/')
			len++;
		if (len == 0 || len >= 255)
			break;
		memcpy(component, p, len);
		component[len] = '\0';

		if (!S_ISDIR(cur->mode)) {
			cur = NULL;
			break;
		}
		cur = tmpfs_dir_lookup(cur, component);
		p += len;
		while (*p == '/')
			p++;
	}

	free(component);
	return cur;
}

/*
 * Walk to the parent directory of path, filling last_component.
 * Returns parent node or NULL on error.
 */
static tmpfs_node *tmpfs_walk_parent(tmpfs_node *root, const char *path,
				     char *last_component)
{
	const char *p = path;
	const char *last_slash = path;

	/* Skip leading slash */
	while (*p == '/')
		p++;

	/* Find last '/' after the start */
	{
		const char *q = p;

		while (*q) {
			if (*q == '/')
				last_slash = q;
			q++;
		}
	}

	strncpy(last_component, last_slash + 1, 255);
	last_component[255] = '\0';

	if (last_slash == path || (last_slash == path && *path == '/')) {
		/* Parent is root */
		return root;
	}

	{
		unsigned parent_len;
		char *parent_path;
		tmpfs_node *parent;

		/* last_slash points into the original path string */
		parent_len = (unsigned)(last_slash - path);
		if (parent_len == 0)
			return root;
		if (parent_len >= 256)
			return NULL;

		parent_path = malloc(parent_len + 1);
		if (!parent_path)
			return NULL;
		memcpy(parent_path, path, parent_len);
		parent_path[parent_len] = '\0';
		parent = tmpfs_walk(root, parent_path);
		free(parent_path);
		return parent;
	}
}

/* ── File inode operations ────────────────────────────────────────────────── */

static int tmpfs_file_getattr(inode *node, struct stat *s)
{
	tmpfs_node *tn = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = tn->mode;
	s->st_ino = (unsigned long)tn->ino;
	s->st_size = (long)tn->size;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = (tn->size + PAGE_SIZE - 1) / PAGE_SIZE;
	s->st_uid = tn->uid;
	s->st_gid = tn->gid;
	s->st_nlink = 1;
	s->st_atime = tn->atime;
	s->st_mtime = tn->mtime;
	s->st_ctime = tn->ctime;
	return 0;
}

static int tmpfs_inode_setattr(inode *node, uint32_t mode)
{
	tmpfs_node *tn = node->i_private;

	tn->mode = (tn->mode & S_IFMT) | (mode & 07777);
	tmpfs_touch_ctime(tn);
	node->i_mode = tn->mode;
	return 0;
}

static int tmpfs_inode_chown(inode *node, uint32_t uid, uint32_t gid)
{
	tmpfs_node *tn = node->i_private;

	if (uid != (uint32_t)-1)
		tn->uid = uid;
	if (gid != (uint32_t)-1)
		tn->gid = gid;
	tmpfs_touch_ctime(tn);
	return 0;
}

static int tmpfs_ensure_page(tmpfs_node *tn, unsigned offset)
{
	unsigned page_idx = offset / PAGE_SIZE;

	if (page_idx < tn->n_pages) {
		if (!tn->pages[page_idx]) {
			tn->pages[page_idx] = zalloc(PAGE_SIZE);
			if (!tn->pages[page_idx])
				return -ENOMEM;
		}
		return 0;
	}

	{
		unsigned new_n = page_idx + 1;
		char **np = malloc(new_n * sizeof(char *));

		if (!np)
			return -ENOMEM;
		if (tn->pages) {
			memcpy(np, tn->pages, tn->n_pages * sizeof(char *));
			free(tn->pages);
		}
		memset(np + tn->n_pages, 0,
		       (new_n - tn->n_pages) * sizeof(char *));
		tn->pages = np;
		tn->n_pages = new_n;

		tn->pages[page_idx] = zalloc(PAGE_SIZE);
		if (!tn->pages[page_idx])
			return -ENOMEM;
	}
	return 0;
}

static int tmpfs_file_read_page(inode *node, unsigned offset, void *buf)
{
	tmpfs_node *tn = node->i_private;
	unsigned page_idx = (offset / PAGE_SIZE);

	if (page_idx >= tn->n_pages || !tn->pages[page_idx]) {
		memset(buf, 0, PAGE_SIZE);
		return 0;
	}
	memcpy(buf, tn->pages[page_idx], PAGE_SIZE);
	return 0;
}

static int tmpfs_file_write_page(inode *node, unsigned offset, const void *buf)
{
	tmpfs_node *tn = node->i_private;
	unsigned page_idx = offset / PAGE_SIZE;
	int ret;

	ret = tmpfs_ensure_page(tn, page_idx * PAGE_SIZE);
	if (ret < 0)
		return ret;

	memcpy(tn->pages[page_idx], buf, PAGE_SIZE);
	if (page_idx * PAGE_SIZE + PAGE_SIZE > tn->size)
		tn->size = page_idx * PAGE_SIZE + PAGE_SIZE;
	tmpfs_touch_mctime(tn);
	node->i_size = tn->size;
	return 0;
}

static int tmpfs_file_ftruncate(inode *node, loff_t size)
{
	tmpfs_node *tn = node->i_private;
	unsigned old_size = tn->size;
	unsigned new_size = (unsigned)size;
	unsigned new_npages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;

	if (size < 0)
		return -EINVAL;

	if (new_size < old_size && new_npages > 0 &&
	    (new_size % PAGE_SIZE) != 0 && new_npages <= tn->n_pages &&
	    tn->pages[new_npages - 1]) {
		memset(tn->pages[new_npages - 1] + (new_size % PAGE_SIZE), 0,
		       PAGE_SIZE - (new_size % PAGE_SIZE));
	}

	if (new_npages < tn->n_pages) {
		unsigned i;

		for (i = new_npages; i < tn->n_pages; i++) {
			free(tn->pages[i]);
			tn->pages[i] = NULL;
		}
		tn->n_pages = new_npages;
	} else if (new_npages > tn->n_pages) {
		char **np = malloc(new_npages * sizeof(char *));

		if (!np)
			return -ENOMEM;
		if (tn->pages) {
			memcpy(np, tn->pages, tn->n_pages * sizeof(char *));
			free(tn->pages);
		}
		memset(np + tn->n_pages, 0,
		       (new_npages - tn->n_pages) * sizeof(char *));
		tn->pages = np;
		tn->n_pages = new_npages;
	}

	if (new_size > old_size && old_size > 0 &&
	    (old_size / PAGE_SIZE) == ((new_size - 1) / PAGE_SIZE) &&
	    (old_size % PAGE_SIZE) != 0 &&
	    (old_size / PAGE_SIZE) < tn->n_pages &&
	    tn->pages[old_size / PAGE_SIZE]) {
		memset(tn->pages[old_size / PAGE_SIZE] + (old_size % PAGE_SIZE),
		       0, new_size - old_size);
	}

	tn->size = new_size;
	tmpfs_touch_mctime(tn);
	node->i_size = tn->size;
	return 0;
}

static const inode_operations tmpfs_file_iops = {
	.getattr = tmpfs_file_getattr,
	.setattr = tmpfs_inode_setattr,
	.chown = tmpfs_inode_chown,
	.read_page = tmpfs_file_read_page,
	.write_page = tmpfs_file_write_page,
	.ftruncate = tmpfs_file_ftruncate,
};

/* ── File file_operations ─────────────────────────────────────────────────── */

static ssize_t tmpfs_file_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	tmpfs_node *tn = fp->f_inode->i_private;
	char *dst = (char *)buf;
	ssize_t transferred = 0;

	if ((unsigned)*pos >= tn->size || size == 0)
		return 0;
	if (*pos + (loff_t)size > (loff_t)tn->size)
		size = (size_t)(tn->size - (unsigned)*pos);

	while ((size_t)transferred < size) {
		unsigned cur_off = (unsigned)*pos + (unsigned)transferred;
		unsigned page_idx = cur_off / PAGE_SIZE;
		unsigned byte_off = cur_off % PAGE_SIZE;
		unsigned avail = PAGE_SIZE - byte_off;
		unsigned want = (unsigned)(size - (size_t)transferred);
		unsigned copy = avail < want ? avail : want;

		if (page_idx >= tn->n_pages || !tn->pages[page_idx])
			memset(dst + transferred, 0, copy);
		else
			memcpy(dst + transferred,
			       tn->pages[page_idx] + byte_off, copy);

		transferred += (ssize_t)copy;
	}

	tn->atime = time_now_sec();
	*pos += transferred;
	return transferred;
}

static ssize_t tmpfs_file_write(file *fp, const void *buf, size_t size,
				loff_t *pos)
{
	tmpfs_node *tn = fp->f_inode->i_private;
	const char *src = (const char *)buf;
	ssize_t transferred = 0;

	while ((size_t)transferred < size) {
		unsigned cur_off = (unsigned)*pos + (unsigned)transferred;
		unsigned page_idx = cur_off / PAGE_SIZE;
		unsigned byte_off = cur_off % PAGE_SIZE;
		unsigned avail = PAGE_SIZE - byte_off;
		unsigned want = (unsigned)(size - (size_t)transferred);
		unsigned copy = avail < want ? avail : want;

		if (tmpfs_ensure_page(tn, cur_off) < 0)
			break;

		if (cur_off > tn->size) {
			unsigned old_size = tn->size;
			unsigned old_page = old_size / PAGE_SIZE;
			unsigned old_off = old_size % PAGE_SIZE;

			if (old_off != 0 && old_page < tn->n_pages &&
			    tn->pages[old_page]) {
				unsigned zero_end = cur_off;

				if (zero_end > (old_page + 1) * PAGE_SIZE)
					zero_end = (old_page + 1) * PAGE_SIZE;
				if (zero_end > old_size)
					memset(tn->pages[old_page] + old_off, 0,
					       zero_end - old_size);
			}
		}

		memcpy(tn->pages[page_idx] + byte_off, src + transferred, copy);
		transferred += (ssize_t)copy;
	}

	if (transferred == 0)
		return -ENOSPC;

	*pos += transferred;
	if ((unsigned)*pos > tn->size)
		tn->size = (unsigned)*pos;
	tmpfs_touch_mctime(tn);
	fp->f_inode->i_size = tn->size;
	return transferred;
}

static loff_t tmpfs_file_llseek(file *fp, loff_t offset, int whence)
{
	tmpfs_node *tn = fp->f_inode->i_private;
	loff_t new_pos;

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = fp->f_pos + offset;
		break;
	case SEEK_END:
		new_pos = (loff_t)tn->size + offset;
		break;
	default:
		return -EINVAL;
	}

	if (new_pos < 0)
		new_pos = 0;

	fp->f_pos = new_pos;
	return fp->f_pos;
}

static int tmpfs_file_release(file *fp)
{
	tmpfs_node_put(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations tmpfs_file_fops = {
	.release = tmpfs_file_release,
	.read = tmpfs_file_read,
	.write = tmpfs_file_write,
	.llseek = tmpfs_file_llseek,
};

/* ── Directory inode/file operations ─────────────────────────────────────── */

static int tmpfs_dir_getattr(inode *node, struct stat *s)
{
	tmpfs_node *tn = node->i_private;
	unsigned nlink = 2;
	list_entry *e;

	for (e = tn->children.next; e != &tn->children; e = e->next) {
		tmpfs_dirent *de = container_of(e, tmpfs_dirent, list);

		if (S_ISDIR(de->node->mode))
			nlink++;
	}

	memset(s, 0, sizeof(*s));
	s->st_mode = tn->mode;
	s->st_ino = (unsigned long)tn->ino;
	s->st_size = PAGE_SIZE;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 1;
	s->st_nlink = nlink;
	s->st_uid = tn->uid;
	s->st_gid = tn->gid;
	s->st_atime = tn->atime;
	s->st_mtime = tn->mtime;
	s->st_ctime = tn->ctime;
	return 0;
}

static const inode_operations tmpfs_dir_iops = {
	.getattr = tmpfs_dir_getattr,
	.setattr = tmpfs_inode_setattr,
	.chown = tmpfs_inode_chown,
};

static ssize_t tmpfs_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	tmpfs_node *dir = fp->f_inode->i_private;
	struct linux_dirent *out = (struct linux_dirent *)buf;
	const char *dot_name;
	unsigned long dot_ino;
	unsigned reclen;
	list_entry *e;
	unsigned off = 0;
	unsigned idx = 0;
	unsigned skip = (unsigned)*pos;

	dot_name = ".";
	dot_ino = (unsigned long)dir->ino;
	reclen = ROUND_UP(NAME_OFFSET() + 2);
	if (skip == 0 && off + reclen <= (unsigned)count) {
		out->d_ino = dot_ino;
		out->d_off = 1;
		out->d_reclen = (unsigned short)reclen;
		memcpy(out->d_name, dot_name, 2);
		out = (struct linux_dirent *)((char *)out + reclen);
		off += reclen;
		idx = 1;
	} else if (skip == 0) {
		*pos = 0;
		return 0;
	} else {
		idx = 1;
	}

	dot_name = "..";
	dot_ino = (unsigned long)(dir->parent ? dir->parent->ino : dir->ino);
	reclen = ROUND_UP(NAME_OFFSET() + 3);
	if (skip <= 1 && off + reclen <= (unsigned)count) {
		out->d_ino = dot_ino;
		out->d_off = 2;
		out->d_reclen = (unsigned short)reclen;
		memcpy(out->d_name, dot_name, 3);
		out = (struct linux_dirent *)((char *)out + reclen);
		off += reclen;
		idx = 2;
	} else if (skip <= 1) {
		*pos = 1;
		return off;
	} else {
		idx = 2;
	}

	for (e = dir->children.next; e != &dir->children; e = e->next) {
		tmpfs_dirent *de = container_of(e, tmpfs_dirent, list);
		unsigned namelen = strlen(de->name);
		unsigned reclen = ROUND_UP(NAME_OFFSET() + namelen + 1);

		if (idx < skip) {
			idx++;
			continue;
		}
		if (off + reclen > (unsigned)count)
			break;

		out->d_ino = (unsigned long)de->node->ino;
		out->d_off = idx + 1;
		out->d_reclen = (unsigned short)reclen;
		memcpy(out->d_name, de->name, namelen + 1);
		out = (struct linux_dirent *)((char *)out + reclen);
		off += reclen;
		idx++;
	}

	*pos = idx;
	dir->atime = time_now_sec();
	return (ssize_t)off;
}

static int tmpfs_dir_release(file *fp)
{
	tmpfs_node_put(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations tmpfs_dir_fops = {
	.release = tmpfs_dir_release,
	.read = tmpfs_dir_read,
};

/* ── Helper: build file * from tmpfs_node ─────────────────────────────────── */

static file *tmpfs_make_file(tmpfs_node *tn)
{
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	tmpfs_node_get(tn);
	node->i_mode = tn->mode;
	node->i_ino = tn->ino;
	node->i_size = tn->size;
	node->i_op = &tmpfs_file_iops;
	node->i_private = tn;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &tmpfs_file_fops;
	return fp;
}

static file *tmpfs_make_dir(tmpfs_node *tn)
{
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	tmpfs_node_get(tn);
	node->i_mode = tn->mode;
	node->i_ino = tn->ino;
	node->i_op = &tmpfs_dir_iops;
	node->i_private = tn;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &tmpfs_dir_fops;
	return fp;
}

/* ── super_operations ─────────────────────────────────────────────────────── */

static file *tmpfs_open_root(super_block *sb, int flag)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;

	return tmpfs_make_dir(sbi->root);
}

static file *tmpfs_open(super_block *sb, const char *path, int flag)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	tmpfs_node *tn = tmpfs_walk(sbi->root, path);

	if (!tn) {
		if (!(flag & O_CREAT))
			return NULL;

		{
			char *last = malloc(256);
			tmpfs_node *parent;
			tmpfs_node *newfile;

			if (!last)
				return NULL;
			parent = tmpfs_walk_parent(sbi->root, path, last);
			if (!parent || !S_ISDIR(parent->mode) || !last[0] ||
			    strcmp(last, ".") == 0 || strcmp(last, "..") == 0) {
				free(last);
				return NULL;
			}
			if (tmpfs_dir_lookup(parent, last)) {
				free(last);
				return NULL;
			}
			newfile = tmpfs_node_alloc(sbi, S_IFREG | 0666);
			tmpfs_dir_add(parent, last, newfile);
			tmpfs_node_put(newfile); /* dir_add bumped it */
			tn = newfile;
			free(last);
		}
	}

	if (S_ISDIR(tn->mode))
		return tmpfs_make_dir(tn);

	{
		file *fp = tmpfs_make_file(tn);

		if (flag & O_TRUNC)
			tmpfs_file_ftruncate(fp->f_inode, 0);
		if (flag & O_APPEND)
			fp->f_pos = tn->size;
		return fp;
	}
}

static int tmpfs_mkdir_op(super_block *sb, const char *path, unsigned mode)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	char *last = malloc(256);
	tmpfs_node *parent;
	tmpfs_node *newdir;

	if (!last)
		return -ENOMEM;

	parent = tmpfs_walk_parent(sbi->root, path, last);
	if (!parent || !S_ISDIR(parent->mode) || !last[0] ||
	    strcmp(last, ".") == 0 || strcmp(last, "..") == 0) {
		free(last);
		return -ENOENT;
	}
	if (tmpfs_dir_lookup(parent, last)) {
		free(last);
		return -EEXIST;
	}

	newdir = tmpfs_node_alloc(sbi, S_IFDIR | (mode & 0777));
	tmpfs_dir_add(parent, last, newdir);
	tmpfs_node_put(newdir);
	free(last);
	return 0;
}

static int tmpfs_unlink_op(super_block *sb, const char *path)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	char *last = malloc(256);
	tmpfs_node *parent;
	int ret;

	if (!last)
		return -ENOMEM;

	parent = tmpfs_walk_parent(sbi->root, path, last);
	if (!parent) {
		free(last);
		return -ENOENT;
	}
	if (!last[0] || strcmp(last, ".") == 0 || strcmp(last, "..") == 0) {
		free(last);
		return -EISDIR;
	}
	{
		tmpfs_node *victim = tmpfs_dir_lookup(parent, last);

		if (!victim) {
			free(last);
			return -ENOENT;
		}
		if (S_ISDIR(victim->mode)) {
			free(last);
			return -EISDIR;
		}
	}
	ret = tmpfs_dir_remove(parent, last);
	free(last);
	return ret;
}

static int tmpfs_rmdir_op(super_block *sb, const char *path)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	char *last = malloc(256);
	tmpfs_node *tn = tmpfs_walk(sbi->root, path);
	tmpfs_node *parent;
	int ret;

	if (!last)
		return -ENOMEM;

	if (!tn) {
		free(last);
		return -ENOENT;
	}
	if (tn == sbi->root || strcmp(path, ".") == 0 ||
	    strcmp(path, "..") == 0) {
		free(last);
		return -EBUSY;
	}
	if (!S_ISDIR(tn->mode)) {
		free(last);
		return -ENOTDIR;
	}
	if (!list_is_empty(&tn->children)) {
		free(last);
		return -ENOTEMPTY;
	}

	parent = tmpfs_walk_parent(sbi->root, path, last);
	if (!parent || !last[0] || strcmp(last, ".") == 0 ||
	    strcmp(last, "..") == 0) {
		free(last);
		return -ENOENT;
	}

	ret = tmpfs_dir_remove(parent, last);
	free(last);
	return ret;
}

static int tmpfs_utime_op(super_block *sb, const char *path, unsigned atime,
			  unsigned mtime)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	tmpfs_node *tn = tmpfs_walk(sbi->root, path);

	if (!tn)
		return -ENOENT;
	tn->atime = atime;
	tn->mtime = mtime;
	tmpfs_touch_ctime(tn);
	return 0;
}

static int tmpfs_statfs_op(super_block *sb, struct statfs *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->f_type = 0x01021994; /* TMPFS_MAGIC */
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = 255;
	return 0;
}

static void tmpfs_free_tree(tmpfs_node *n)
{
	list_entry *e;

	if (S_ISDIR(n->mode)) {
		for (e = n->children.next; e != &n->children;) {
			tmpfs_dirent *de = container_of(e, tmpfs_dirent, list);

			e = e->next;
			tmpfs_free_tree(de->node);
			free(de->name);
			free(de);
		}
	}
	tmpfs_node_put(n);
}

static void tmpfs_release_super(super_block *sb)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;

	if (sbi) {
		if (sbi->root)
			tmpfs_free_tree(sbi->root);
		free(sbi);
	}
	free(sb);
}

static super_operations tmpfs_sops = {
	.open_root = tmpfs_open_root,
	.open = tmpfs_open,
	.mkdir = tmpfs_mkdir_op,
	.rmdir = tmpfs_rmdir_op,
	.unlink = tmpfs_unlink_op,
	.statfs = tmpfs_statfs_op,
	.utime = tmpfs_utime_op,
	.release = tmpfs_release_super,
};

/* ── get_sb — called from mount.c ────────────────────────────────────────── */

static super_block *tmpfs_get_sb(const char *dev, const char *target, int flags,
				 void *data)
{
	super_block *sb = sget(&tmpfs_sops);
	tmpfs_sb_info *sbi = zalloc(sizeof(*sbi));

	sbi->next_ino = 1;
	spinlock_init(&sbi->lock);
	sbi->root = tmpfs_node_alloc(sbi, S_IFDIR | 0777);
	sbi->root->parent = sbi->root;
	sb->s_fs_info = sbi;
	return sb;
}

static fs_type tmpfs_fs_type = { .name = "tmpfs", .get_sb = tmpfs_get_sb };

static void tmpfs_fs_type_init(void)
{
	printk("mnt: registered tmpfs file type\n");
	fs_register_type(&tmpfs_fs_type);
}

KERNEL_INIT(2, tmpfs_fs_type_init);
