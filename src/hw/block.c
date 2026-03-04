#include "ext4.h"
#include <macro.h>
#include <block.h>
#include <lock.h>
#include <klib.h>
#include <fs.h>

static struct _block_control {
	list_entry block_lists[BLOCK_MAX];
	unsigned int block_count[BLOCK_MAX];
	spinlock_t lock;
} control;

static unsigned int id;

static void block_add_to_list(block *b, block_type type)
{
	list_entry *head = &control.block_lists[type];

	spinlock_lock(&control.lock);
	list_insert_tail(head, &(b->block_list));
	control.block_count[type] = control.block_count[type] + 1;
	spinlock_unlock(&control.lock);
}

static block *block_get_first(block_type type)
{
	list_entry *head = &control.block_lists[type];
	list_entry *node = 0;
	block *ret = 0;

	if (list_is_empty(head))
		return 0;

	spinlock_lock(&control.lock);

	node = head->prev;

	spinlock_unlock(&control.lock);

	if (node == head)
		return 0;
	else
		return (container_of(node, block, block_list));
}

static block *block_get_next(block *b)
{
	list_entry *head = &control.block_lists[b->type];
	list_entry *node = 0;

	spinlock_lock(&control.lock);

	node = b->block_list.prev;

	spinlock_unlock(&control.lock);

	if (node == head)
		return 0;
	else
		return (container_of(node, block, block_list));
}

static unsigned int block_id_gen()
{
	unsigned int ret;
	spinlock_lock(&control.lock);
	ret = id;
	id++;
	spinlock_unlock(&control.lock);
	return ret;
}

void block_init()
{
	int i = 0;

	for (i = 0; i < BLOCK_MAX; i++) {
		list_init(&control.block_lists[i]);
		control.block_count[i] = 0;
	}

	spinlock_init(&control.lock);

	id = 0;
}

block *block_register(void *aux, char *name, fpblock_read read,
		      fpblock_write write, fpblock_close close, block_type type,
		      unsigned int sector_size)
{
	block *b = kmalloc(sizeof(*b));

	memset(b, 0, sizeof(*b));
	b->aux = aux;
	if (name && *name)
		strcpy(b->name, name);
	else
		*b->name = '\0';

	b->read = read;
	b->write = write;
	b->close = close;
	b->type = type;
	b->sector_size = sector_size;
	b->id = block_id_gen();

	block_add_to_list(b, type);

	return b;
}

block *block_get_by_id(unsigned int id)
{
	int i = 0;
	block *b = 0;

	for (i = 0; i < BLOCK_MAX; i++) {
		for (b = block_get_first((block_type)i); b;
		     b = block_get_next(b)) {
			if (b->id == id)
				return b;
		}
	}

	return 0;
}

unsigned int block_get_by_type(block_type type, block **blocks)
{
	block *b = 0;
	int i = 0;
	int count = control.block_count[type];

	if (!blocks)
		return count;

	for (b = block_get_first(type), i = 0; (b) && (i < count);
	     b = block_get_next(b), i++) {
		blocks[i] = b;
	}

	return count;
}

void block_close()
{
	int i = 0;
	block *b = 0;

	for (i = 0; i < BLOCK_MAX; i++) {
		for (b = block_get_first((block_type)i); b;
		     b = block_get_next(b)) {
			if (b->close) {
				b->close(b->aux);
			}
		}
	}
}

const char *block_type_name(block *b)
{
	switch (b->type) {
	case BLOCK_KERNEL:
		return "BLOCK_KERNEL";
	case BLOCK_LINUX:
		return "BLOCK_LINUX";
	case BLOCK_FILESYS:
		return "BLOCK_FILESYS";
	case BLOCK_SCRATCH:
		return "BLOCK_SCRATCH";
	case BLOCK_SWAP:
		return "BLOCK_SWAP";
	case BLOCK_RAW:
		return "BLOCK_RAW";
	case BLOCK_UNKNOW:
	default:
		return "BLOCK_UNKNOW";
	}
}

unsigned fs_read_size = 0;
unsigned fs_write_size = 0;

static int ext4_file_release(inode *node, file *fp)
{
	ext4_file *f = node->i_private;
	ext4_fclose(f);
	free(f);
	free(node);
	return 0;
}

static ssize_t ext4_file_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	ext4_file *f = fp->f_inode->i_private;
	size_t rcnt = 0;
	/* Sync ext4 cursor with f_pos if they diverged (e.g. after pread) */
	if ((loff_t)ext4_ftell(f) != *pos)
		ext4_fseek(f, *pos, SEEK_SET);
	int ret = ext4_fread(f, buf, size, &rcnt);
	fs_read_size += rcnt;
	if (ret != EOK)
		return -1;
	*pos += rcnt;
	return (ssize_t)rcnt;
}

static ssize_t ext4_file_write(file *fp, const void *buf, size_t size,
			       loff_t *pos)
{
	ext4_file *f = fp->f_inode->i_private;
	size_t wcnt = 0;
	if ((loff_t)ext4_ftell(f) != *pos)
		ext4_fseek(f, *pos, SEEK_SET);
	int ret = ext4_fwrite(f, buf, size, &wcnt);
	fs_write_size += wcnt;
	if (ret != EOK)
		return -1;
	*pos += wcnt;
	return (ssize_t)wcnt;
}

static loff_t ext4_file_llseek(file *fp, loff_t offset, int whence)
{
	ext4_file *f = fp->f_inode->i_private;
	int ret;
	switch (whence) {
	case SEEK_SET:
		if ((uint64_t)offset > f->fsize) {
			ret = ext4_fenlarge(f, offset);
			if (ret != EOK)
				return (loff_t)(0 - ret);
		}
		break;
	case SEEK_CUR:
		if ((uint64_t)(offset + f->fpos) > f->fsize) {
			ret = ext4_fenlarge(f, offset + f->fpos);
			if (ret != EOK)
				return (loff_t)(0 - ret);
		}
		break;
	case SEEK_END:
		if ((uint64_t)offset > f->fsize)
			return -EINVAL;
		break;
	}
	ret = ext4_fseek(f, offset, whence);
	if (ret != EOK)
		return (loff_t)(0 - ret);
	return (loff_t)ext4_ftell(f);
}

static int ext4_file_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT)
		return -1;
	return 0;
}

static int ext4_file_getattr(inode *node, struct stat *s)
{
	ext4_file *f = node->i_private;
	return ext4_fstat(f, s);
}

static int ext4_file_setattr(inode *node, uint32_t mode)
{
	ext4_file *f = node->i_private;
	return ext4_fchmod(f, mode);
}

static const inode_operations ext4_file_iops = {
	.getattr = ext4_file_getattr,
	.setattr = ext4_file_setattr,
};

static const file_operations ext4_file_fops = {
	.release = ext4_file_release,
	.read = ext4_file_read,
	.write = ext4_file_write,
	.llseek = ext4_file_llseek,
	.poll = ext4_file_poll,
};

static int ext4_dir_release(inode *node, file *fp)
{
	ext4_dir *dir = node->i_private;
	ext4_dir_close(dir);
	free(dir);
	free(node);
	return 0;
}

static ssize_t ext4_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	ext4_dir *dir = fp->f_inode->i_private;
	struct linux_dirent *dirp = buf;
	ext4_direntry *entry = NULL;
	struct linux_dirent *prev = NULL;
	int retcount = 0;
	int len;
	int cur_pos = 0;

	while (count > 0) {
		entry = ext4_dir_entry_next(dir);
		if (entry == NULL) {
			if (prev)
				prev->d_off = retcount;
			break;
		}
		len = ROUND_UP(NAME_OFFSET() + strlen(entry->name) + 1);
		if (count < len) {
			if (prev)
				prev->d_off = retcount;
			break;
		}
		memset(dirp, 0, len);
		dirp->d_ino = entry->inode;
		strncpy(dirp->d_name, entry->name, entry->name_length);
		dirp->d_reclen =
			ROUND_UP(NAME_OFFSET() + strlen(dirp->d_name) + 1);
		cur_pos += dirp->d_reclen;
		dirp->d_off = cur_pos;
		retcount += dirp->d_reclen;
		count -= dirp->d_reclen;
		prev = dirp;
		dirp = (char *)dirp + dirp->d_reclen;
	}
	*pos += retcount;
	return (ssize_t)retcount;
}

static loff_t ext4_dir_llseek(file *fp, loff_t offset, int whence)
{
	ext4_dir *dir = fp->f_inode->i_private;
	ext4_direntry *entry = NULL;
	int len;
	int cur_pos = 0;
	int count = (int)offset;

	if (whence != SEEK_SET)
		return -EACCES;

	if (offset < (loff_t)sizeof(struct linux_dirent))
		return 0;

	ext4_dir_entry_rewind(dir);
	while (count > 0) {
		entry = ext4_dir_entry_next(dir);
		if (entry == NULL)
			break;
		len = ROUND_UP(NAME_OFFSET() + strlen(entry->name) + 1);
		if (count < len)
			return (loff_t)(cur_pos + len);
		cur_pos += len;
		count -= len;
	}
	return (loff_t)cur_pos;
}

static int ext4_dir_getattr(inode *node, struct stat *s)
{
	ext4_dir *dir = node->i_private;
	return ext4_fstat(&dir->f, s);
}

static const inode_operations ext4_dir_iops = {
	.getattr = ext4_dir_getattr,
};

static const file_operations ext4_dir_fops = {
	.release = ext4_dir_release,
	.read = ext4_dir_read,
	.llseek = ext4_dir_llseek,
	.poll = ext4_file_poll,
};

file *fs_alloc_filep_normal(void *content)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_op = &ext4_file_iops;
	node->i_fop = &ext4_file_fops;
	node->i_private = content;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_op = &ext4_file_fops;
	fp->f_count = 1;
	return fp;
}

file *fs_alloc_filep_dir(void *content)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_op = &ext4_dir_iops;
	node->i_fop = &ext4_dir_fops;
	node->i_private = content;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_op = &ext4_dir_fops;
	fp->f_count = 1;
	return fp;
}
