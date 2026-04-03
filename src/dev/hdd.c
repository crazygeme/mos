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
	loff_t dev_size = (loff_t)pi->size * BLOCK_SECTOR_SIZE;
	char *tmp;
	ssize_t transferred = 0;

	if (*pos >= dev_size || size == 0)
		return 0;
	if (*pos + (loff_t)size > dev_size)
		size = (size_t)(dev_size - *pos);

	tmp = malloc(BLOCK_SECTOR_SIZE);
	if (!tmp)
		return -ENOMEM;

	while ((size_t)transferred < size) {
		unsigned sector =
			(unsigned)((*pos + transferred) / BLOCK_SECTOR_SIZE);
		unsigned byte_off =
			(unsigned)((*pos + transferred) % BLOCK_SECTOR_SIZE);
		unsigned avail = BLOCK_SECTOR_SIZE - byte_off;
		unsigned want = (unsigned)(size - (size_t)transferred);
		unsigned copy = avail < want ? avail : want;

		if (sector >= pi->size)
			break;

		/* Aligned full-sector: read directly into dst. */
		if (byte_off == 0 && copy == BLOCK_SECTOR_SIZE) {
			if (pi->read(pi->aux, sector, dst + transferred,
				     BLOCK_SECTOR_SIZE) < 0)
				break;
		} else {
			if (pi->read(pi->aux, sector, tmp, BLOCK_SECTOR_SIZE) <
			    0)
				break;
			memcpy(dst + transferred, tmp + byte_off, copy);
		}
		transferred += (ssize_t)copy;
	}

	free(tmp);
	*pos += transferred;
	return transferred;
}

static ssize_t hdd_dev_write(file *fp, const void *buf, size_t size,
			     loff_t *pos)
{
	hdd_partition_info *pi = fp->f_inode->i_private;
	const char *src = (const char *)buf;
	loff_t dev_size = (loff_t)pi->size * BLOCK_SECTOR_SIZE;
	char *tmp;
	ssize_t transferred = 0;

	if (*pos >= dev_size || size == 0)
		return 0;
	if (*pos + (loff_t)size > dev_size)
		size = (size_t)(dev_size - *pos);

	tmp = malloc(BLOCK_SECTOR_SIZE);
	if (!tmp)
		return -ENOMEM;

	while ((size_t)transferred < size) {
		unsigned sector =
			(unsigned)((*pos + transferred) / BLOCK_SECTOR_SIZE);
		unsigned byte_off =
			(unsigned)((*pos + transferred) % BLOCK_SECTOR_SIZE);
		unsigned avail = BLOCK_SECTOR_SIZE - byte_off;
		unsigned want = (unsigned)(size - (size_t)transferred);
		unsigned copy = avail < want ? avail : want;

		if (sector >= pi->size)
			break;

		/* Aligned full-sector: write directly from src. */
		if (byte_off == 0 && copy == BLOCK_SECTOR_SIZE) {
			if (pi->write(pi->aux, sector,
				      (char *)src + transferred,
				      BLOCK_SECTOR_SIZE) < 0)
				break;
		} else {
			/* Read-modify-write for partial sectors. */
			if (pi->read(pi->aux, sector, tmp, BLOCK_SECTOR_SIZE) <
			    0)
				break;
			memcpy(tmp + byte_off, src + transferred, copy);
			if (pi->write(pi->aux, sector, tmp, BLOCK_SECTOR_SIZE) <
			    0)
				break;
		}
		transferred += (ssize_t)copy;
	}

	free(tmp);
	*pos += transferred;
	return transferred;
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
	s->st_size =
		0; /* block devices report 0 in stat; use llseek(SEEK_END) */
	s->st_blksize = BLOCK_SECTOR_SIZE;
	s->st_blocks = (loff_t)pi->size;
	s->st_dev = MKDEV(HDD_MAJOR, 0);
	s->st_rdev = MKDEV(HDD_MAJOR, idx + 1);
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_ctime = time_now_sec();
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
		printk("dev: registered /dev%s (%u sectors)\n", path, pi->size);
	}
}

DEV_INIT(hdd_dev_register);
