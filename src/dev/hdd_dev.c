/*
 * src/dev/hdd_dev.c - /dev/hdXN block device nodes
 *
 * For every partition discovered by hdd.c a block device node is created at
 * /dev/<name> via vfs_mknod.  The cdev dispatch table maps MKDEV(3, index)
 * to hdd_cdev_open(), which wires up the partition's read/write callbacks.
 *
 * Device numbering (Linux-compatible IDE):
 *   major 3, minor 1 = first partition (hda1), minor 2 = hda2, …
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <hw/hdd.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <macro.h>
#include <dev/dev.h>
#include <unistd.h>
#include <ext4.h>

#define HDD_MAJOR 3

/* ── File operations ─────────────────────────────────────────────────────── */

static ssize_t hdd_dev_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	hdd_partition_info *pi = fp->f_inode->i_private;
	char *dst = (char *)buf;
	unsigned sector = (unsigned)(*pos / BLOCK_SECTOR_SIZE);
	unsigned nsectors = (unsigned)(size / BLOCK_SECTOR_SIZE);
	unsigned i;

	for (i = 0; i < nsectors; i++, dst += BLOCK_SECTOR_SIZE) {
		if (sector + i >= pi->size)
			break;
		if (pi->read(pi->aux, sector + i, dst, BLOCK_SECTOR_SIZE) < 0)
			break;
	}

	*pos += (loff_t)i * BLOCK_SECTOR_SIZE;
	return (ssize_t)i * BLOCK_SECTOR_SIZE;
}

static ssize_t hdd_dev_write(file *fp, const void *buf, size_t size,
			     loff_t *pos)
{
	hdd_partition_info *pi = fp->f_inode->i_private;
	const char *src = (const char *)buf;
	unsigned sector = (unsigned)(*pos / BLOCK_SECTOR_SIZE);
	unsigned nsectors = (unsigned)(size / BLOCK_SECTOR_SIZE);
	unsigned i;

	for (i = 0; i < nsectors; i++, src += BLOCK_SECTOR_SIZE) {
		if (sector + i >= pi->size)
			break;
		if (pi->write(pi->aux, sector + i, (char *)src,
			      BLOCK_SECTOR_SIZE) < 0)
			break;
	}

	*pos += (loff_t)i * BLOCK_SECTOR_SIZE;
	return (ssize_t)i * BLOCK_SECTOR_SIZE;
}

static loff_t hdd_dev_llseek(file *fp, loff_t offset, int whence)
{
	hdd_partition_info *pi = fp->f_inode->i_private;
	loff_t dev_size = (loff_t)pi->size * BLOCK_SECTOR_SIZE;
	loff_t new_pos;

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = fp->f_pos + offset;
		break;
	case SEEK_END:
		new_pos = dev_size + offset;
		break;
	default:
		return -EINVAL;
	}

	if (new_pos < 0)
		new_pos = 0;
	if (new_pos > dev_size)
		new_pos = dev_size;

	fp->f_pos = new_pos;
	return fp->f_pos;
}

static int hdd_dev_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT)
		return -1;
	return 0;
}

static int hdd_dev_getattr(inode *node, struct stat *s)
{
	hdd_partition_info *pi = node->i_private;
	unsigned idx = (unsigned)(pi - hdd_partitions);

	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = (loff_t)pi->size * BLOCK_SECTOR_SIZE;
	s->st_blksize = BLOCK_SECTOR_SIZE;
	s->st_blocks = (loff_t)pi->size;
	s->st_dev = MKDEV(HDD_MAJOR, 0);
	s->st_rdev = MKDEV(HDD_MAJOR, idx + 1);
	s->st_nlink = 1;
	s->st_atime = time_now_ms();
	return 0;
}

static int hdd_dev_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations hdd_dev_iops = {
	.getattr = hdd_dev_getattr,
};

static const file_operations hdd_dev_fops = {
	.release = hdd_dev_release,
	.read = hdd_dev_read,
	.write = hdd_dev_write,
	.llseek = hdd_dev_llseek,
	.is_ready = hdd_dev_poll,
};

/* ── cdev dispatch ───────────────────────────────────────────────────────── */

static file *hdd_cdev_open(unsigned rdev, int flag)
{
	unsigned idx = MINOR(rdev) - 1; /* minor 1 = hda1, minor 2 = hda2, … */

	if ((int)idx >= hdd_partition_count)
		return NULL;

	hdd_partition_info *pi = &hdd_partitions[idx];

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	node->i_op = &hdd_dev_iops;
	node->i_private = pi;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &hdd_dev_fops;
	return fp;
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

static void hdd_dev_register(super_block *dev_sb)
{
	char path[48];
	int i;

	cdev_register(S_IFBLK, HDD_MAJOR, 1, (unsigned)hdd_partition_count,
		      hdd_cdev_open);

	for (i = 0; i < hdd_partition_count; i++) {
		hdd_partition_info *pi = &hdd_partitions[i];
		sprintf(path, "/%s", pi->name);
		vfs_mknod(dev_sb, path, S_IFBLK | 0660,
			  MKDEV(HDD_MAJOR, i + 1));
		printk("hdd_dev: registered /dev%s (%u sectors)\n", path,
		       pi->size);
	}
}

DEV_INIT(hdd_dev_register);
