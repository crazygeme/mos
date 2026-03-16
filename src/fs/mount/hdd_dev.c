/*
 * src/fs/mount/hdd_dev.c - /dev/hdXN block device nodes
 *
 * For every partition discovered by hdd.c a character-like block device is
 * mounted at /dev/<name> (e.g. /dev/hda1, /dev/hda2).  The file operations
 * translate byte-level read/write requests into sector-granular calls through
 * the partition's read/write callbacks stored in hdd_partition_info.
 *
 * Reads and writes that cross sector boundaries or are not sector-aligned are
 * handled via a one-sector temporary buffer.
 */

#include <fs/fs.h>
#include <fs/mount.h>
#include <hw/hdd.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <macro.h>
#include <unistd.h>
#include <errno.h>
#include <ext4.h>

/* ── File operations ─────────────────────────────────────────────────────── */

static ssize_t hdd_dev_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	hdd_partition_info *pi = fp->f_inode->i_private;
	char *dst = (char *)buf;
	loff_t off = *pos;
	size_t remaining = size;
	char tmp[BLOCK_SECTOR_SIZE];

	while (remaining > 0) {
		unsigned sector = (unsigned)(off / BLOCK_SECTOR_SIZE);
		unsigned sector_off = (unsigned)(off % BLOCK_SECTOR_SIZE);
		unsigned chunk;

		if (sector >= pi->size)
			break;

		chunk = BLOCK_SECTOR_SIZE - sector_off;
		if (chunk > remaining)
			chunk = (unsigned)remaining;

		if (pi->read(pi->aux, sector, tmp, BLOCK_SECTOR_SIZE) < 0)
			break;

		memcpy(dst, tmp + sector_off, chunk);
		dst += chunk;
		off += chunk;
		remaining -= chunk;
	}

	*pos = off;
	return (ssize_t)(size - remaining);
}

static ssize_t hdd_dev_write(file *fp, const void *buf, size_t size,
			     loff_t *pos)
{
	hdd_partition_info *pi = fp->f_inode->i_private;
	const char *src = (const char *)buf;
	loff_t off = *pos;
	size_t remaining = size;
	char tmp[BLOCK_SECTOR_SIZE];

	while (remaining > 0) {
		unsigned sector = (unsigned)(off / BLOCK_SECTOR_SIZE);
		unsigned sector_off = (unsigned)(off % BLOCK_SECTOR_SIZE);
		unsigned chunk;

		if (sector >= pi->size)
			break;

		chunk = BLOCK_SECTOR_SIZE - sector_off;
		if (chunk > remaining)
			chunk = (unsigned)remaining;

		/* Read-modify-write for partial sector updates. */
		if (sector_off != 0 || chunk < BLOCK_SECTOR_SIZE) {
			if (pi->read(pi->aux, sector, tmp, BLOCK_SECTOR_SIZE) <
			    0)
				break;
		}

		memcpy(tmp + sector_off, src, chunk);

		if (pi->write(pi->aux, sector, tmp, BLOCK_SECTOR_SIZE) < 0)
			break;

		src += chunk;
		off += chunk;
		remaining -= chunk;
	}

	*pos = off;
	return (ssize_t)(size - remaining);
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
	s->st_mode = node->i_mode;
	s->st_size = (loff_t)pi->size * BLOCK_SECTOR_SIZE;
	s->st_blksize = BLOCK_SECTOR_SIZE;
	s->st_blocks = (loff_t)pi->size;
	s->st_dev = 0x8; /* major 8 = sd/hd */
	s->st_rdev = 0x8;
	s->st_ino = 0;
	s->st_nlink = 1;
	s->st_uid = 0;
	s->st_gid = 0;
	s->st_atime = time_now_ms();
	s->st_mtime = 0;
	s->st_ctime = 0;
	return 0;
}

static const inode_operations hdd_dev_iops = {
	.getattr = hdd_dev_getattr,
};

static const file_operations hdd_dev_fops = {
	.read = hdd_dev_read,
	.write = hdd_dev_write,
	.llseek = hdd_dev_llseek,
	.poll = hdd_dev_poll,
};

/* ── Superblock operations ───────────────────────────────────────────────── */

static file *hdd_dev_open_root(super_block *sb)
{
	hdd_partition_info *pi = sb->s_fs_info;

	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	node->i_op = &hdd_dev_iops;
	node->i_private = pi;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &hdd_dev_fops;
	return fp;
}

static super_operations hdd_dev_sops = {
	.open_root = hdd_dev_open_root,
};

/* ── Initialisation ──────────────────────────────────────────────────────── */

static void hdd_dev_init(void)
{
	task_struct *cur = CURRENT_TASK();
	int i;
	char mount_path[48];

	for (i = 0; i < hdd_partition_count; i++) {
		hdd_partition_info *pi = &hdd_partitions[i];
		super_block *sb = sget(&hdd_dev_sops);
		sb->s_fs_info = pi;
		sprintf(mount_path, "/dev/%s", pi->name);
		vfs_mount(cur->root, mount_path, sb);
		printk("hdd_dev: mounted %s (%u sectors)\n", mount_path,
		       pi->size);
	}
}

KERNEL_INIT(5, hdd_dev_init);
