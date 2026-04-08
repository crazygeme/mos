#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <macro.h>
#include <dev/dev.h>
#include <unistd.h>
#include <ext4_oflags.h>
#include <errno.h>

/* /dev/mem — major 1, minor 1 (Linux-compatible) */
#define MEM_MAJOR 1
#define MEM_MINOR 1

int dev_mem_is_file(const file *fp)
{
	return fp && fp->f_inode &&
	       (unsigned)(uintptr_t)fp->f_inode->i_private ==
		       MKDEV(MEM_MAJOR, MEM_MINOR);
}

static unsigned mem_dev_limit(void)
{
	return phymm_end * PAGE_SIZE;
}

static int mem_copy_from_phys(void *dst, unsigned phys, size_t size)
{
	size_t done = 0;

	while (done < size) {
		unsigned cur = phys + (unsigned)done;
		unsigned base = cur & PAGE_SIZE_MASK;
		unsigned page_off = cur & ~PAGE_SIZE_MASK;
		size_t chunk = PAGE_SIZE - page_off;

		if (chunk > size - done)
			chunk = size - done;
		if (mm_kmap_phys(base) != 1)
			return -EIO;
		memcpy((char *)dst + done,
		       (void *)(PHY_TO_VIRT(base) + page_off), chunk);
		done += chunk;
	}

	return 0;
}

static int mem_copy_to_phys(unsigned phys, const void *src, size_t size)
{
	size_t done = 0;

	while (done < size) {
		unsigned cur = phys + (unsigned)done;
		unsigned base = cur & PAGE_SIZE_MASK;
		unsigned page_off = cur & ~PAGE_SIZE_MASK;
		size_t chunk = PAGE_SIZE - page_off;

		if (chunk > size - done)
			chunk = size - done;
		if (mm_kmap_phys(base) != 1)
			return -EIO;
		memcpy((void *)(PHY_TO_VIRT(base) + page_off),
		       (const char *)src + done, chunk);
		done += chunk;
	}

	return 0;
}

static ssize_t mem_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	unsigned limit = mem_dev_limit();
	size_t avail;

	if (!buf || size == 0)
		return 0;
	if (*pos < 0)
		return -EINVAL;
	if ((unsigned)*pos >= limit)
		return 0;

	avail = limit - (unsigned)*pos;
	if (size > avail)
		size = avail;
	if (mem_copy_from_phys(buf, (unsigned)*pos, size) != 0)
		return -EIO;

	*pos += (loff_t)size;
	return (ssize_t)size;
}

static ssize_t mem_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	unsigned limit = mem_dev_limit();
	size_t avail;

	if (!buf || size == 0)
		return 0;
	if (*pos < 0)
		return -EINVAL;
	if ((unsigned)*pos >= limit)
		return -ENOSPC;

	avail = limit - (unsigned)*pos;
	if (size > avail)
		size = avail;
	if (mem_copy_to_phys((unsigned)*pos, buf, size) != 0)
		return -EIO;

	*pos += (loff_t)size;
	return (ssize_t)size;
}

static loff_t mem_llseek(file *fp, loff_t offset, int whence)
{
	loff_t limit = (loff_t)mem_dev_limit();
	loff_t new_pos;

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = fp->f_pos + offset;
		break;
	case SEEK_END:
		new_pos = limit + offset;
		break;
	default:
		return -EINVAL;
	}

	if (new_pos < 0)
		return -EINVAL;
	if (new_pos > limit)
		new_pos = limit;

	fp->f_pos = new_pos;
	return new_pos;
}

static unsigned mem_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return events & (FS_POLL_READ | FS_POLL_WRITE);
}

static int mem_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = (unsigned)(uintptr_t)node->i_private;
	s->st_size = (loff_t)mem_dev_limit();
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = (loff_t)((mem_dev_limit() + 511) / 512);
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	return 0;
}

static int mem_read_page(inode *node, unsigned offset, void *buf)
{
	(void)node;
	memset(buf, 0, PAGE_SIZE);
	if (offset >= mem_dev_limit())
		return 0;
	if (offset + PAGE_SIZE > mem_dev_limit())
		return mem_copy_from_phys(buf, offset,
					  mem_dev_limit() - offset);
	return mem_copy_from_phys(buf, offset, PAGE_SIZE);
}

static int mem_write_page(inode *node, unsigned offset, const void *buf)
{
	(void)node;
	if (offset >= mem_dev_limit())
		return 0;
	if (offset + PAGE_SIZE > mem_dev_limit())
		return mem_copy_to_phys(offset, buf, mem_dev_limit() - offset);
	return mem_copy_to_phys(offset, buf, PAGE_SIZE);
}

static int mem_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations mem_iops = {
	.getattr = mem_getattr,
	.read_page = mem_read_page,
	.write_page = mem_write_page,
};

static const file_operations mem_fops = {
	.release = mem_release,
	.read = mem_read,
	.write = mem_write,
	.llseek = mem_llseek,
	.poll = mem_poll,
};

static file *mem_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	(void)dev_sb;
	(void)rdev;
	(void)flag;

	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR;
	node->i_private = (void *)(uintptr_t)MKDEV(MEM_MAJOR, MEM_MINOR);
	node->i_op = &mem_iops;

	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &mem_fops;
	return fp;
}

static void mem_dev_register(super_block *dev_sb)
{
	printk("dev: registered /dev/mem\n");
	cdev_register(S_IFCHR, MEM_MAJOR, MEM_MINOR, 1, mem_cdev_open);
	vfs_mknod(dev_sb, "/mem", S_IFCHR | 0600, MKDEV(MEM_MAJOR, MEM_MINOR));
}

DEV_INIT(mem_dev_register);
