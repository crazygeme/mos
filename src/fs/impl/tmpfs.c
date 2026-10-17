/*
 * src/fs/tmpfs.c - in-memory temporary filesystem
 *
 * Provides volatile files, directories, and symbolic links for runtime
 * and shared-memory mounts.
 *
 * Files store data in lazily-allocated PAGE_SIZE chunks.
 * Directories hold a doubly-linked list of (name, node) dentries.
 * All state lives in kernel heap; nothing persists across reboots.
 */

#include <fs/fs.h>
#include <fs/cache.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <fs/fcntl.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <int/int.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/list.h>
#include <ps/ps.h>
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
	paddr_t *pages;
	unsigned n_pages;
	rmutex_t file_lock;
	tmpfs_node *parent;
	/* Dirs: sentinel head of child dirent list */
	list_entry children;
	unsigned ref, links;
	char *symlink;
	rmutex_t *tree_lock;
};

/* ── Per-superblock info ──────────────────────────────────────────────────── */

typedef struct {
	tmpfs_node *root;
	unsigned next_ino;
	spinlock_t lock;
	rmutex_t directory_lock;
} tmpfs_sb_info;

static int tmpfs_page_read(paddr_t page, unsigned offset, void *out,
			   unsigned size)
{
	if (mm_kmap_phys(page) != 1)
		return -EIO;
	memcpy(out, (char *)PHY_TO_VIRT(page) + offset, size);
	mm_kunmap_phys(page);
	return 0;
}

static int tmpfs_page_write(paddr_t page, unsigned offset, const void *in,
			    unsigned size)
{
	if (mm_kmap_phys(page) != 1)
		return -EIO;
	memcpy((char *)PHY_TO_VIRT(page) + offset, in, size);
	mm_kunmap_phys(page);
	return 0;
}

static int tmpfs_page_zero(paddr_t page, unsigned offset, unsigned size)
{
	if (mm_kmap_phys(page) != 1)
		return -EIO;
	memset((char *)PHY_TO_VIRT(page) + offset, 0, size);
	mm_kunmap_phys(page);
	return 0;
}

static void tmpfs_page_put(paddr_t page)
{
	if (page && phymm_dereference_page(page / PAGE_SIZE) == 0)
		phymm_free_user(page / PAGE_SIZE);
}

static paddr_t tmpfs_page_alloc(void)
{
	unsigned index = phymm_alloc_user();
	if (index == PHYMM_INVALID) {
		phymm_reclaim_user_cache(32);
		index = phymm_alloc_user();
		if (index == PHYMM_INVALID)
			return 0;
	}
	paddr_t page = (paddr_t)index * PAGE_SIZE;
	phymm_reference_page(index);
	if (tmpfs_page_zero(page, 0, PAGE_SIZE) < 0) {
		tmpfs_page_put(page);
		return 0;
	}
	return page;
}

/* ── Node lifecycle ───────────────────────────────────────────────────────── */

static tmpfs_node *tmpfs_node_alloc(tmpfs_sb_info *sbi, uint32_t mode)
{
	tmpfs_node *n = zalloc(sizeof(*n));
	task_struct *cur = CURRENT_TASK();
	unsigned now = time_wall_sec();
	int irq;

	spinlock_lock(&sbi->lock, &irq);
	n->ino = sbi->next_ino++;
	spinlock_unlock(&sbi->lock, irq);

	n->mode = mode;
	if (cur && cur->fs) {
		n->uid = cur->credentials->euid;
		n->gid = cur->credentials->egid;
	}
	n->ref = 1;
	n->tree_lock = &sbi->directory_lock;
	vm_lock_init(&n->file_lock);
	n->atime = n->mtime = n->ctime = now;
	list_init(&n->children);
	return n;
}

static void tmpfs_touch_mctime(tmpfs_node *n)
{
	unsigned now = time_wall_sec();

	n->mtime = now;
	n->ctime = now;
}

static void tmpfs_touch_ctime(tmpfs_node *n)
{
	n->ctime = time_wall_sec();
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
			tmpfs_page_put(n->pages[i]);
		free(n->pages);
	}
	free(n->symlink);
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

static int tmpfs_dir_add(tmpfs_node *dir, const char *name, tmpfs_node *node)
{
	tmpfs_dirent *de = zalloc(sizeof(*de));
	if (!de)
		return -ENOMEM;
	de->name = strdup(name);
	if (!de->name) {
		free(de);
		return -ENOMEM;
	}
	de->node = node;
	node->parent = dir;
	tmpfs_node_get(node);
	if (!S_ISDIR(node->mode))
		node->links++;
	list_insert_tail(&dir->children, &de->list);
	tmpfs_touch_mctime(dir);
	return 0;
}

static int tmpfs_dir_remove(tmpfs_node *dir, const char *name)
{
	list_entry *e;

	for (e = dir->children.next; e != &dir->children; e = e->next) {
		tmpfs_dirent *de = container_of(e, tmpfs_dirent, list);

		if (strcmp(de->name, name) == 0) {
			list_remove_entry(e);
			if (!S_ISDIR(de->node->mode) && de->node->links)
				de->node->links--;
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
		if (len == 0 || len > 255) {
			cur = NULL;
			break;
		}
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
	const char *begin = path, *end = path + strlen(path);
	while (*begin == '/')
		begin++;
	while (end > begin && end[-1] == '/')
		end--;
	last_component[0] = 0;
	if (end == begin)
		return root;
	const char *component = end;
	while (component > begin && component[-1] != '/')
		component--;
	size_t length = end - component;
	if (length > 255)
		return NULL;
	memcpy(last_component, component, length);
	last_component[length] = 0;
	if (component == begin)
		return root;
	size_t parent_length = component - path;
	if (parent_length >= MAX_PATH)
		return NULL;
	char *parent_path = name_get();
	if (!parent_path)
		return NULL;
	memcpy(parent_path, path, parent_length);
	parent_path[parent_length] = 0;
	tmpfs_node *parent = tmpfs_walk(root, parent_path);
	name_put(parent_path);
	return parent;
}

/* ── File inode operations ────────────────────────────────────────────────── */

static int tmpfs_file_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	tmpfs_node *tn = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = tn->mode;
	s->st_ino = (unsigned long)tn->ino;
	s->st_size = (long)tn->size;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	for (unsigned i = 0; i < tn->n_pages; i++)
		if (tn->pages[i])
			s->st_blocks += PAGE_SIZE / 512;
	s->st_uid = tn->uid;
	s->st_gid = tn->gid;
	s->st_nlink = tn->links;
	s->st_atime = tn->atime;
	s->st_mtime = tn->mtime;
	s->st_ctime = tn->ctime;
	return 0;
}

static int tmpfs_inode_setattr(file *fp, uint32_t mode)
{
	inode *node = fp->f_inode;
	tmpfs_node *tn = node->i_private;

	tn->mode = (tn->mode & S_IFMT) | (mode & 07777);
	tmpfs_touch_ctime(tn);
	node->i_mode = tn->mode;
	return 0;
}

static int tmpfs_inode_chown(file *fp, uint32_t uid, uint32_t gid)
{
	inode *node = fp->f_inode;
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
			tn->pages[page_idx] = tmpfs_page_alloc();
			if (!tn->pages[page_idx])
				return -ENOMEM;
		}
		return 0;
	}

	{
		unsigned new_n = page_idx + 1;
		paddr_t *np = malloc(new_n * sizeof(paddr_t));

		if (!np)
			return -ENOMEM;
		if (tn->pages) {
			memcpy(np, tn->pages, tn->n_pages * sizeof(paddr_t));
			free(tn->pages);
		}
		memset(np + tn->n_pages, 0,
		       (new_n - tn->n_pages) * sizeof(paddr_t));
		tn->pages = np;
		tn->n_pages = new_n;

		tn->pages[page_idx] = tmpfs_page_alloc();
		if (!tn->pages[page_idx])
			return -ENOMEM;
	}
	return 0;
}

static int tmpfs_page_resident(file *fp, uint64_t offset)
{
	tmpfs_node *tn = fp->f_inode->i_private;
	LOCK_GUARD(&tn->file_lock);
	uint64_t index = offset / PAGE_SIZE;
	return offset < tn->size && index < tn->n_pages &&
	       tn->pages[index] != 0;
}
static paddr_t tmpfs_file_map_page(file *fp, uint64_t offset)
{
	tmpfs_node *tn = fp->f_inode->i_private;
	LOCK_GUARD(&tn->file_lock);
	if (offset >= tn->size || offset > UINT32_MAX ||
	    tmpfs_ensure_page(tn, (unsigned)offset) < 0)
		return 0;
	paddr_t page = tn->pages[offset / PAGE_SIZE];
	phymm_reference_page(page / PAGE_SIZE);
	return page;
}

static void tmpfs_file_map_page_put(file *fp, paddr_t page)
{
	(void)fp;
	tmpfs_page_put(page);
}

static int tmpfs_file_read_page(file *fp, uint64_t offset, void *buf)
{
	if (offset >= 0x100000000ULL) {
		memset(buf, 0, PAGE_SIZE);
		return 0;
	}
	inode *node = fp->f_inode;
	tmpfs_node *tn = node->i_private;
	LOCK_GUARD(&tn->file_lock);
	unsigned page_idx = (offset / PAGE_SIZE);

	if (page_idx >= tn->n_pages || !tn->pages[page_idx]) {
		memset(buf, 0, PAGE_SIZE);
		return 0;
	}
	return tmpfs_page_read(tn->pages[page_idx], 0, buf, PAGE_SIZE);
}

static int tmpfs_file_write_page(file *fp, uint64_t offset, const void *buf)
{
	if (offset > 0xffffffffULL - PAGE_SIZE)
		return -EFBIG;
	inode *node = fp->f_inode;
	tmpfs_node *tn = node->i_private;
	LOCK_GUARD(&tn->file_lock);
	unsigned page_idx = offset / PAGE_SIZE;
	int ret;

	fs_page_cache_invalidate(fp);
	ret = tmpfs_ensure_page(tn, page_idx * PAGE_SIZE);
	if (ret < 0)
		return ret;

	if (tmpfs_page_write(tn->pages[page_idx], 0, buf, PAGE_SIZE) < 0)
		return -EIO;
	if (page_idx * PAGE_SIZE + PAGE_SIZE > tn->size)
		tn->size = page_idx * PAGE_SIZE + PAGE_SIZE;
	tmpfs_touch_mctime(tn);
	node->i_size = tn->size;
	return 0;
}

static int tmpfs_file_ftruncate(file *fp, loff_t size)
{
	inode *node = fp->f_inode;
	tmpfs_node *tn = node->i_private;
	LOCK_GUARD(&tn->file_lock);
	unsigned old_size = tn->size;
	unsigned new_size = (unsigned)size;
	unsigned new_npages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;

	if (size < 0)
		return -EINVAL;
	if ((uint64_t)size > 0xffffffffULL)
		return -EFBIG;

	fs_page_cache_invalidate(fp);

	if (new_size < old_size && new_npages > 0 &&
	    (new_size % PAGE_SIZE) != 0 && new_npages <= tn->n_pages &&
	    tn->pages[new_npages - 1]) {
		if (tmpfs_page_zero(tn->pages[new_npages - 1],
				    new_size % PAGE_SIZE,
				    PAGE_SIZE - (new_size % PAGE_SIZE)) < 0)
			return -EIO;
	}

	if (new_npages < tn->n_pages) {
		unsigned i;

		for (i = new_npages; i < tn->n_pages; i++) {
			tmpfs_page_put(tn->pages[i]);
			tn->pages[i] = NULL;
		}
		tn->n_pages = new_npages;
	} else if (new_npages > tn->n_pages) {
		paddr_t *np = malloc(new_npages * sizeof(paddr_t));

		if (!np)
			return -ENOMEM;
		if (tn->pages) {
			memcpy(np, tn->pages, tn->n_pages * sizeof(paddr_t));
			free(tn->pages);
		}
		memset(np + tn->n_pages, 0,
		       (new_npages - tn->n_pages) * sizeof(paddr_t));
		tn->pages = np;
		tn->n_pages = new_npages;
	}

	if (new_size > old_size && old_size > 0 &&
	    (old_size / PAGE_SIZE) == ((new_size - 1) / PAGE_SIZE) &&
	    (old_size % PAGE_SIZE) != 0 &&
	    (old_size / PAGE_SIZE) < tn->n_pages &&
	    tn->pages[old_size / PAGE_SIZE]) {
		if (tmpfs_page_zero(tn->pages[old_size / PAGE_SIZE],
				    old_size % PAGE_SIZE,
				    new_size - old_size) < 0)
			return -EIO;
	}

	tn->size = new_size;
	tmpfs_touch_mctime(tn);
	node->i_size = tn->size;
	return 0;
}

static int tmpfs_file_fallocate(file *fp, int mode, uint64_t offset,
				uint64_t length)
{
	tmpfs_node *tn = fp->f_inode->i_private;
	LOCK_GUARD(&tn->file_lock);
	uint64_t end = offset + length;
	if (end > 0xffffffffULL)
		return -EFBIG;
	if (!(mode & 1) && end > tn->size) {
		int ret = tmpfs_file_ftruncate(fp, end);
		if (ret)
			return ret;
	}
	for (uint64_t page = offset / PAGE_SIZE; page <= (end - 1) / PAGE_SIZE;
	     page++) {
		int ret = tmpfs_ensure_page(tn, page * PAGE_SIZE);
		if (ret)
			return ret;
	}
	return 0;
}

/* ── File file_operations ─────────────────────────────────────────────────── */

static ssize_t tmpfs_file_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	tmpfs_node *tn = fp->f_inode->i_private;
	LOCK_GUARD(&tn->file_lock);
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
		else if (tmpfs_page_read(tn->pages[page_idx], byte_off,
					 dst + transferred, copy) < 0)
			return transferred ? transferred : -EIO;

		transferred += (ssize_t)copy;
	}

	tn->atime = time_wall_sec();
	*pos += transferred;
	return transferred;
}

static ssize_t tmpfs_file_write(file *fp, const void *buf, size_t size,
				loff_t *pos)
{
	if (!size)
		return 0;
	tmpfs_node *tn = fp->f_inode->i_private;
	LOCK_GUARD(&tn->file_lock);
	const char *src = (const char *)buf;
	ssize_t transferred = 0;

	fs_page_cache_invalidate(fp);

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
					if (tmpfs_page_zero(
						    tn->pages[old_page],
						    old_off,
						    zero_end - old_size) < 0)
						return transferred ?
							       transferred :
							       -EIO;
			}
		}

		if (tmpfs_page_write(tn->pages[page_idx], byte_off,
				     src + transferred, copy) < 0)
			return transferred ? transferred : -EIO;
		if (cur_off + copy > tn->size)
			tn->size = cur_off + copy;
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
	free(fp);
	return 0;
}

static int tmpfs_file_readlink(file *fp, char *buf, size_t size, size_t *length)
{
	tmpfs_node *node = fp->f_inode->i_private;
	if (!node->symlink)
		return -EINVAL;
	unsigned n = strlen(node->symlink);
	if (n > size)
		n = size;
	memcpy(buf, node->symlink, n);
	*length = n;
	return 0;
}

static file *tmpfs_file_reopen(file *fp, int flags);

static const file_operations tmpfs_file_fops = {
	.reopen = tmpfs_file_reopen,
	.map_page = tmpfs_file_map_page,
	.page_resident = tmpfs_page_resident,
	.map_page_put = tmpfs_file_map_page_put,
	.release = tmpfs_file_release,
	.getattr = tmpfs_file_getattr,
	.readlink = tmpfs_file_readlink,
	.setattr = tmpfs_inode_setattr,
	.chown = tmpfs_inode_chown,
	.read = tmpfs_file_read,
	.write = tmpfs_file_write,
	.llseek = tmpfs_file_llseek,
	.read_page = tmpfs_file_read_page,
	.write_page = tmpfs_file_write_page,
	.ftruncate = tmpfs_file_ftruncate,
	.fallocate = tmpfs_file_fallocate,
};

/* ── Directory inode/file operations ─────────────────────────────────────── */

static int tmpfs_dir_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	tmpfs_node *tn = node->i_private;
	LOCK_GUARD(tn->tree_lock);
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
	dir->atime = time_wall_sec();
	return (ssize_t)off;
}

static int tmpfs_dir_release(file *fp)
{
	tmpfs_node_put(fp->f_inode->i_private);
	free(fp);
	return 0;
}

static const file_operations tmpfs_dir_fops = {
	.release = tmpfs_dir_release,
	.getattr = tmpfs_dir_getattr,
	.setattr = tmpfs_inode_setattr,
	.chown = tmpfs_inode_chown,
	.read = tmpfs_dir_read,
};

/* ── Helper: build file * from tmpfs_node ─────────────────────────────────── */

/* The file is first so release can free the description and inode together. */
typedef struct {
	file file;
	inode inode;
} tmpfs_open_file;

static file *tmpfs_make_file(tmpfs_node *tn, super_block *sb, const char *path)
{
	size_t base = strlen(sb->s_mountpoint), length = strlen(path);
	int inline_name = base < sizeof(sb->s_mountpoint) - 1;
	if (base == 1 && sb->s_mountpoint[0] == '/')
		base = 0;
	tmpfs_open_file *opened =
		malloc(sizeof(*opened) + (inline_name ? base + length + 1 : 0));
	if (!opened)
		return NULL;
	memset(opened, 0, sizeof(*opened));
	inode *node = &opened->inode;
	file *fp = &opened->file;
	if (inline_name) {
		fp->f_name = (char *)(opened + 1);
		fp->f_name_inline = 1;
		memcpy(fp->f_name, sb->s_mountpoint, base);
		memcpy(fp->f_name + base, path, length + 1);
	}

	tmpfs_node_get(tn);
	node->i_mode = tn->mode;
	node->i_ino = tn->ino;
	node->i_size = tn->size;
	node->i_pgcache_tag = tn;
	node->i_private = tn;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &tmpfs_file_fops;
	return fp;
}

static file *tmpfs_file_reopen(file *fp, int flags)
{
	if (!fp->f_sb)
		return NULL;
	tmpfs_node *tn = fp->f_inode->i_private;
	LOCK_GUARD(&tn->file_lock);
	file *out = tmpfs_make_file(tn, fp->f_sb, "");
	if (!out)
		return NULL;
	if (out->f_name && !out->f_name_inline)
		free(out->f_name);
	out->f_name = fp->f_name ? strdup(fp->f_name) : NULL;
	out->f_name_inline = 0;
	out->f_sb = fp->f_sb;
	sb_get(out->f_sb);
	out->f_mount_id = fp->f_mount_id;
	out->f_mount_flags = fp->f_mount_flags;
	out->f_mount_root = fp->f_mount_root;
	out->f_flag = flags;
	out->f_mode = flags & O_ACCMODE;
	if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY)
		tmpfs_file_ftruncate(out, 0);
	return out;
}

static file *tmpfs_make_dir(tmpfs_node *tn)
{
	tmpfs_open_file *opened = zalloc(sizeof(*opened));
	if (!opened)
		return NULL;
	inode *node = &opened->inode;
	file *fp = &opened->file;

	tmpfs_node_get(tn);
	node->i_mode = tn->mode;
	node->i_ino = tn->ino;
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
	LOCK_GUARD(&sbi->directory_lock);

	return tmpfs_make_dir(sbi->root);
}

static file *tmpfs_open_direct(super_block *sb, const char *path, int flag)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
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
			if (tmpfs_dir_add(parent, last, newfile)) {
				tmpfs_node_put(newfile);
				free(last);
				return NULL;
			}
			tmpfs_node_put(newfile); /* dir_add bumped it */
			tn = newfile;
			free(last);
		}
	}

	if (S_ISDIR(tn->mode))
		return tmpfs_make_dir(tn);

	{
		file *fp = tmpfs_make_file(tn, sb, path);
		if (!fp)
			return NULL;

		if (flag & O_TRUNC)
			tmpfs_file_ftruncate(fp, 0);
		if (flag & O_APPEND)
			fp->f_pos = tn->size;
		return fp;
	}
}

static file *tmpfs_open_link_locked(super_block *sb, const char *path,
				    int flags, char **target, int follow_final)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
	tmpfs_node *node = sbi->root;
	const char *p = path;
	while (*p == '/')
		p++;
	while (*p) {
		const char *begin = p;
		while (*p && *p != '/')
			p++;
		unsigned n = p - begin;
		if (!n || n > 255 || !S_ISDIR(node->mode))
			return NULL;
		char name[256];
		memcpy(name, begin, n);
		name[n] = 0;
		node = tmpfs_dir_lookup(node, name);
		if (!node)
			return tmpfs_open_direct(sb, path, flags);
		if (node->symlink && (*p || follow_final)) {
			char *text = name_get(), *link = name_get();
			if (!text || !link) {
				if (text)
					name_put(text);
				if (link)
					name_put(link);
				return NULL;
			}
			unsigned base = !strcmp(sb->s_mountpoint, "/") ?
						0 :
						strlen(sb->s_mountpoint);
			unsigned prefix = p - path;
			if (base + prefix >= MAX_PATH) {
				name_put(text);
				name_put(link);
				return NULL;
			}
			memcpy(link, sb->s_mountpoint, base);
			memcpy(link + base, path, prefix);
			link[base + prefix] = 0;
			strcpy(text, node->symlink);
			if (fs_resolve_symlink_path(link, text, strlen(text)) ||
			    strlen(text) + strlen(p) >= MAX_PATH) {
				name_put(text);
				name_put(link);
				return NULL;
			}
			strcat(text, p);
			name_put(link);
			*target = text;
			return NULL;
		}
		while (*p == '/')
			p++;
	}
	return tmpfs_open_direct(sb, path, flags);
}
static file *tmpfs_open(super_block *sb, const char *path, int flags)
{
	char *target = NULL;
	file *fp = tmpfs_open_link_locked(sb, path, flags, &target,
					  !(flags & O_NOFOLLOW));
	if (target) {
		fp = vfs_open(current->fs->root, target, flags);
		name_put(target);
	}
	return fp;
}
static file *tmpfs_open_link(super_block *sb, const char *path, int flags,
			     char **target)
{
	return tmpfs_open_link_locked(sb, path, flags, target, 1);
}
static int tmpfs_symlink_op(super_block *sb, const char *target,
			    const char *path)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
	char name[256];
	tmpfs_node *parent = tmpfs_walk_parent(sbi->root, path, name);
	if (!parent || !S_ISDIR(parent->mode) || !*name || !strcmp(name, ".") ||
	    !strcmp(name, ".."))
		return -ENOENT;
	if (tmpfs_dir_lookup(parent, name))
		return -EEXIST;
	if (strlen(target) >= MAX_PATH)
		return -ENAMETOOLONG;
	tmpfs_node *node = tmpfs_node_alloc(sbi, S_IFLNK | 0777);
	if (!node)
		return -ENOMEM;
	node->symlink = strdup(target);
	if (!node->symlink) {
		tmpfs_node_put(node);
		return -ENOMEM;
	}
	node->size = strlen(target);
	tmpfs_dir_add(parent, name, node);
	tmpfs_node_put(node);
	return 0;
}
static int tmpfs_readlink_op(super_block *sb, const char *path, char *buf,
			     size_t size, size_t *length)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
	tmpfs_node *node = tmpfs_walk(sbi->root, path);
	if (!node)
		return -ENOENT;
	if (!node->symlink)
		return -EINVAL;
	unsigned n = strlen(node->symlink);
	if (n > size)
		n = size;
	memcpy(buf, node->symlink, n);
	*length = n;
	return 0;
}

static int tmpfs_mkdir_op(super_block *sb, const char *path, unsigned mode)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
	if (tmpfs_walk(sbi->root, path))
		return -EEXIST;
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

static int tmpfs_link_op(super_block *sb, const char *oldpath,
			 const char *newpath)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
	tmpfs_node *node = tmpfs_walk(sbi->root, oldpath);
	if (!node)
		return -ENOENT;
	if (S_ISDIR(node->mode))
		return -EPERM;
	char name[256];
	tmpfs_node *parent = tmpfs_walk_parent(sbi->root, newpath, name);
	if (!parent || !S_ISDIR(parent->mode))
		return -ENOENT;
	if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
		return -EEXIST;
	if (tmpfs_dir_lookup(parent, name))
		return -EEXIST;
	int ret = tmpfs_dir_add(parent, name, node);
	if (!ret)
		tmpfs_touch_ctime(node);
	return ret;
}

static int tmpfs_unlink_op(super_block *sb, const char *path)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
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
	LOCK_GUARD(&sbi->directory_lock);
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

static int tmpfs_rename_op(super_block *sb, const char *oldpath,
			   const char *newpath)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
	char oldname[256], newname[256];
	tmpfs_node *oldparent = tmpfs_walk_parent(sbi->root, oldpath, oldname);
	tmpfs_node *newparent = tmpfs_walk_parent(sbi->root, newpath, newname);
	if (!oldparent || !newparent)
		return -ENOENT;
	if (!*oldname || !*newname || !strcmp(oldname, ".") ||
	    !strcmp(oldname, "..") || !strcmp(newname, ".") ||
	    !strcmp(newname, ".."))
		return -EINVAL;
	tmpfs_node *node = tmpfs_dir_lookup(oldparent, oldname);
	if (!node)
		return -ENOENT;
	if (oldparent == newparent && !strcmp(oldname, newname))
		return 0;
	if (S_ISDIR(node->mode)) {
		for (tmpfs_node *p = newparent;; p = p->parent) {
			if (p == node)
				return -EINVAL;
			if (!p->parent || p == p->parent)
				break;
		}
	}
	tmpfs_node *target = tmpfs_dir_lookup(newparent, newname);
	if (target == node)
		return 0;
	if (target) {
		if (S_ISDIR(target->mode) && !S_ISDIR(node->mode))
			return -EISDIR;
		if (!S_ISDIR(target->mode) && S_ISDIR(node->mode))
			return -ENOTDIR;
		if (S_ISDIR(target->mode) && !list_is_empty(&target->children))
			return -ENOTEMPTY;
	}
	char *name = strdup(newname);
	if (!name)
		return -ENOMEM;
	tmpfs_dirent *entry = NULL;
	for (list_entry *p = oldparent->children.next;
	     p != &oldparent->children; p = p->next) {
		tmpfs_dirent *candidate = container_of(p, tmpfs_dirent, list);
		if (!strcmp(candidate->name, oldname)) {
			entry = candidate;
			break;
		}
	}
	if (!entry) {
		free(name);
		return -ENOENT;
	}
	if (target)
		tmpfs_dir_remove(newparent, newname);
	list_remove_entry(&entry->list);
	free(entry->name);
	entry->name = name;
	node->parent = newparent;
	list_insert_tail(&newparent->children, &entry->list);
	tmpfs_touch_mctime(oldparent);
	tmpfs_touch_mctime(newparent);
	tmpfs_touch_ctime(node);
	return 0;
}

static int tmpfs_utime_op(super_block *sb, const char *path, unsigned atime,
			  unsigned mtime)
{
	tmpfs_sb_info *sbi = sb->s_fs_info;
	LOCK_GUARD(&sbi->directory_lock);
	tmpfs_node *tn = tmpfs_walk(sbi->root, path);

	if (!tn)
		return -ENOENT;
	tn->atime = atime;
	tn->mtime = mtime;
	tmpfs_touch_ctime(tn);
	return 0;
}

static int tmpfs_statfs_op(super_block *sb, struct statfs64 *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->f_type = 0x01021994; /* TMPFS_MAGIC */
	buf->f_bsize = PAGE_SIZE;
	phymm_usage usage;
	phymm_get_usage(&usage);
	buf->f_blocks =
		(uint64_t)usage.low_total_pages + usage.high_total_pages;
	buf->f_bfree = (uint64_t)usage.low_free_pages + usage.high_free_pages;
	buf->f_bavail = buf->f_bfree;
	buf->f_frsize = PAGE_SIZE;
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
	.open_link = tmpfs_open_link,
	.symlink = tmpfs_symlink_op,
	.readlink = tmpfs_readlink_op,
	.mkdir = tmpfs_mkdir_op,
	.rmdir = tmpfs_rmdir_op,
	.unlink = tmpfs_unlink_op,
	.link = tmpfs_link_op,
	.rename = tmpfs_rename_op,
	.statfs = tmpfs_statfs_op,
	.utime = tmpfs_utime_op,
	.release = tmpfs_release_super,
};

/* ── get_sb — called from mount.c ────────────────────────────────────────── */

static int tmpfs_root_options(tmpfs_node *node, const char *data)
{
	while (data && *data) {
		const char *end = strchr(data, ',');
		if (!end)
			end = data + strlen(data);
		unsigned *field = NULL, base = 10;
		const char *value = NULL;
		if (!strncmp(data, "mode=", 5)) {
			field = &node->mode;
			base = 8;
			value = data + 5;
		} else if (!strncmp(data, "uid=", 4)) {
			field = &node->uid;
			value = data + 4;
		} else if (!strncmp(data, "gid=", 4)) {
			field = &node->gid;
			value = data + 4;
		}
		if (field) {
			uint64_t number = 0;
			if (value == end)
				return -EINVAL;
			for (; value < end; value++) {
				unsigned digit = (unsigned char)*value - '0';
				if (digit >= base ||
				    number > (UINT32_MAX - digit) / base)
					return -EINVAL;
				number = number * base + digit;
			}
			if (field == &node->mode) {
				if (number > 07777)
					return -EINVAL;
				*field = S_IFDIR | number;
			} else
				*field = number;
		}
		data = *end ? end + 1 : end;
	}
	return 0;
}

static super_block *tmpfs_get_sb(const char *dev, const char *target, int flags,
				 void *data)
{
	super_block *sb = sget(&tmpfs_sops);
	tmpfs_sb_info *sbi = zalloc(sizeof(*sbi));

	sbi->next_ino = 1;
	spinlock_init(&sbi->lock);
	vm_lock_init(&sbi->directory_lock);
	sbi->root = tmpfs_node_alloc(sbi, S_IFDIR | 0777);
	sbi->root->parent = sbi->root;
	sb->s_fs_info = sbi;
	if (tmpfs_root_options(sbi->root, data)) {
		tmpfs_release_super(sb);
		return NULL;
	}
	return sb;
}

static fs_type tmpfs_fs_type = { .name = "tmpfs", .get_sb = tmpfs_get_sb };

static void tmpfs_fs_type_init(void)
{
	printk("mnt: registered tmpfs file type\n");
	fs_register_type(&tmpfs_fs_type);
}

KERNEL_INIT(2, tmpfs_fs_type_init);
