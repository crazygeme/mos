/*
 * src/dev/loopdev.c — loop block device
 *
 * Supports two usage modes:
 *
 *   Auto-loop (via mount):
 *     mount("/path/to/img", "/mnt", "ext3", 0, NULL)
 *     ext4_get_sb sees a non-device path → calls loop_setup(path) → "loop0"
 *
 *   Manual (losetup-compatible):
 *     losetup /dev/loop0 /path/to/img
 *       open("/dev/loop0", O_RDWR) → loop_cdev_open (cdev major=7)
 *       ioctl(fd, LOOP_SET_FD, img_fd)
 *     mount /dev/loop0 /mnt -t ext3
 *     losetup -d /dev/loop0
 *       ioctl(fd, LOOP_CLR_FD, 0)
 *
 * /dev/loop0..7 are pre-created at boot by loop_dev_register (DEV_INIT).
 */
#include <dev/loopdev.h>
#include <dev/blockdev.h>
#include <dev/dev.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <mm/mm.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <ext4_blockdev.h>
#include <ext4.h>
#include <errno.h>
#include "devnums.h"

/* ── Loop ioctl numbers (linux/loop.h) ──────────────────────────────────────── */
#define LOOP_SET_FD 0x4C00
#define LOOP_CLR_FD 0x4C01
#define LOOP_SET_STATUS 0x4C02
#define LOOP_GET_STATUS 0x4C03
#define LOOP_SET_STATUS64 0x4C04
#define LOOP_GET_STATUS64 0x4C05

#define LO_NAME_SIZE 64
#define LO_KEY_SIZE 32

/* struct loop_info — used by LOOP_{GET,SET}_STATUS (old 32-bit ioctl) */
struct loop_info {
	int lo_number;
	unsigned short lo_device;
	unsigned long lo_inode;
	unsigned short lo_rdevice;
	int lo_offset;
	int lo_encrypt_type;
	int lo_encrypt_key_size;
	int lo_flags;
	char lo_name[LO_NAME_SIZE];
	unsigned char lo_encrypt_key[LO_KEY_SIZE];
	unsigned long lo_init[2];
	char reserved[4];
};

/* struct loop_info64 — used by LOOP_{GET,SET}_STATUS64 */
struct loop_info64 {
	uint64_t lo_device;
	uint64_t lo_inode;
	uint64_t lo_rdevice;
	uint64_t lo_offset;
	uint64_t lo_sizelimit;
	uint32_t lo_number;
	uint32_t lo_encrypt_type;
	uint32_t lo_encrypt_key_size;
	uint32_t lo_flags;
	uint8_t lo_file_name[LO_NAME_SIZE];
	uint8_t lo_crypt_name[LO_NAME_SIZE];
	uint8_t lo_encrypt_key[LO_KEY_SIZE];
	uint64_t lo_init[2];
};

/* ── Per-slot state ─────────────────────────────────────────────────────────── */

loop_dev_info loop_devs[LOOP_MAX_DEVS];

typedef struct {
	file *fp;
	struct ext4_blockdev_iface bdif;
	struct ext4_blockdev bdev;
} loop_device;

static loop_device loop_devices[LOOP_MAX_DEVS];

/* ── lwext4 block device callbacks ─────────────────────────────────────────── */

static int loop_bdev_open(struct ext4_blockdev *bdev)
{
	(void)bdev;
	return 0;
}

static int loop_bdev_close(struct ext4_blockdev *bdev)
{
	/* No-op: bdev+bdif are embedded in the static pool. */
	(void)bdev;
	return 0;
}

static int loop_bdev_bread(struct ext4_blockdev *bdev, void *buf,
			   uint64_t blk_id, uint32_t blk_cnt)
{
	loop_device *ld = bdev->aux;
	loff_t off = (loff_t)blk_id * bdev->bdif->ph_bsize;
	size_t n = (size_t)blk_cnt * bdev->bdif->ph_bsize;
	ssize_t r = ld->fp->f_fop->read(ld->fp, buf, n, &off);
	return (r == (ssize_t)n) ? EOK : EIO;
}

static int loop_bdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			    uint64_t blk_id, uint32_t blk_cnt)
{
	loop_device *ld = bdev->aux;
	loff_t off = (loff_t)blk_id * bdev->bdif->ph_bsize;
	size_t n = (size_t)blk_cnt * bdev->bdif->ph_bsize;
	ssize_t r = ld->fp->f_fop->write(ld->fp, (void *)buf, n, &off);
	return (r == (ssize_t)n) ? EOK : EIO;
}

static int loop_bdev_lock(struct ext4_blockdev *bdev)
{
	(void)bdev;
	return 0;
}
static int loop_bdev_unlock(struct ext4_blockdev *bdev)
{
	(void)bdev;
	return 0;
}

/* ── Internal: attach a file to a slot ─────────────────────────────────────── */

static int loop_attach(int i, file *fp, const char *path_hint)
{
	loop_device *ld = &loop_devices[i];
	loop_dev_info *info = &loop_devs[i];
	uint64_t size = fp->f_inode ? fp->f_inode->i_size : 0;

	if (!size) {
		/* Fall back to stat */
		struct stat st;
		if (fs_stat(path_hint ? path_hint : "", &st) == 0)
			size = (uint64_t)st.st_size;
	}
	if (!size)
		return -EINVAL;

	ld->fp = fp;

	memset(&ld->bdif, 0, sizeof(ld->bdif));
	ld->bdif.open = loop_bdev_open;
	ld->bdif.bread = loop_bdev_bread;
	ld->bdif.bwrite = loop_bdev_bwrite;
	ld->bdif.close = loop_bdev_close;
	ld->bdif.lock = loop_bdev_lock;
	ld->bdif.unlock = loop_bdev_unlock;
	ld->bdif.ph_bsize = 512;
	ld->bdif.ph_bcnt = (uint32_t)(size / 512);
	ld->bdif.ph_bbuf = (uint8_t *)kmalloc(512);

	memset(&ld->bdev, 0, sizeof(ld->bdev));
	ld->bdev.bdif = &ld->bdif;
	ld->bdev.part_offset = 0;
	ld->bdev.part_size = size;
	ld->bdev.aux = ld;

	sprintf(info->name, "loop%d", i);
	if (path_hint)
		strncpy(info->backing, path_hint, sizeof(info->backing) - 1);
	info->size_bytes = size;

	ext4_device_register(&ld->bdev, NULL, info->name);
	blockdev_update(info->name, size, BLOCKDEV_FLAG_MOUNTABLE);
	return 0;
}

static void loop_detach(int i)
{
	loop_device *ld = &loop_devices[i];
	loop_dev_info *info = &loop_devs[i];

	if (!info->name[0])
		return;

	ext4_device_unregister(info->name);
	kfree(ld->bdif.ph_bbuf);
	fs_put_file(ld->fp);
	ld->fp = NULL;
	/* Keep name[] so /dev/loop0 still shows in directory; clear backing. */
	info->backing[0] = '\0';
	info->size_bytes = 0;
	blockdev_update(info->name, 0, 0);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

const char *loop_setup(const char *path)
{
	int i;
	file *fp;
	struct stat st;

	for (i = 0; i < LOOP_MAX_DEVS; i++) {
		if (!loop_devs[i].backing[0])
			break;
	}
	if (i == LOOP_MAX_DEVS) {
		klog("loop: no free slots\n");
		return NULL;
	}

	fp = fs_open_file(path, O_RDWR, 0);
	if (!fp) {
		klog("loop: cannot open %s\n", path);
		return NULL;
	}

	if (fs_stat(path, &st) < 0 || st.st_size == 0) {
		fs_put_file(fp);
		klog("loop: stat failed or empty: %s\n", path);
		return NULL;
	}

	if (loop_attach(i, fp, path) < 0) {
		fs_put_file(fp);
		return NULL;
	}
	return loop_devs[i].name;
}

void loop_teardown(const char *name)
{
	int i;
	for (i = 0; i < LOOP_MAX_DEVS; i++) {
		if (strcmp(loop_devs[i].name, name) == 0) {
			loop_detach(i);
			return;
		}
	}
	klog("loop: teardown: %s not found\n", name);
}

/* ── cdev interface — open /dev/loopN ──────────────────────────────────────── */

static int loop_ioctl(file *fp, unsigned cmd, void *buf)
{
	int minor = (int)(uintptr_t)fp->f_inode->i_private;
	task_struct *cur = CURRENT_TASK();
	uint64_t size_bytes = loop_devs[minor].size_bytes;

	switch (cmd) {
	case BLKGETSIZE:
		if (!buf)
			return -EINVAL;
		if (!loop_devices[minor].fp)
			return -ENXIO;
		*(unsigned long *)buf = (unsigned long)(size_bytes / 512);
		return 0;

	case BLKGETSIZE64:
		if (!buf)
			return -EINVAL;
		if (!loop_devices[minor].fp)
			return -ENXIO;
		*(uint64_t *)buf = size_bytes;
		return 0;

	case BLKSSZGET:
		if (!buf)
			return -EINVAL;
		*(int *)buf = 512;
		return 0;

	case LOOP_SET_FD: {
		int img_fd = (int)(uintptr_t)buf;
		file *img_fp;

		if (loop_devs[minor].backing[0])
			return -EBUSY;
		if (img_fd < 0 || img_fd >= (int)MAX_FD || !cur->fds[img_fd])
			return -EBADF;

		img_fp = cur->fds[img_fd];
		fs_get_file(img_fp);
		if (loop_attach(minor, img_fp, NULL) < 0) {
			fs_put_file(img_fp);
			return -EINVAL;
		}
		return 0;
	}

	case LOOP_CLR_FD:
		if (!loop_devs[minor].backing[0] &&
		    loop_devices[minor].fp == NULL)
			return -ENXIO;
		loop_detach(minor);
		return 0;

	case LOOP_SET_STATUS: {
		struct loop_info *li = (struct loop_info *)buf;
		if (!li)
			return -EINVAL;
		/* Accept but only store the backing name if provided */
		if (li->lo_name[0])
			strncpy(loop_devs[minor].backing, li->lo_name,
				sizeof(loop_devs[minor].backing) - 1);
		return 0;
	}

	case LOOP_GET_STATUS: {
		struct loop_info *li = (struct loop_info *)buf;
		if (!li)
			return -EINVAL;
		if (!loop_devs[minor].backing[0])
			return -ENXIO;
		memset(li, 0, sizeof(*li));
		li->lo_number = minor;
		li->lo_device = (unsigned short)MKDEV(3, 1); /* hda1 */
		li->lo_rdevice = (unsigned short)MKDEV(LOOP_MAJOR, minor);
		strncpy(li->lo_name, loop_devs[minor].backing,
			LO_NAME_SIZE - 1);
		return 0;
	}

	case LOOP_SET_STATUS64: {
		struct loop_info64 *li = (struct loop_info64 *)buf;
		if (!li)
			return -EINVAL;
		if (li->lo_file_name[0])
			strncpy(loop_devs[minor].backing,
				(char *)li->lo_file_name,
				sizeof(loop_devs[minor].backing) - 1);
		return 0;
	}

	case LOOP_GET_STATUS64: {
		struct loop_info64 *li = (struct loop_info64 *)buf;
		if (!li)
			return -EINVAL;
		if (!loop_devs[minor].backing[0])
			return -ENXIO;
		memset(li, 0, sizeof(*li));
		li->lo_number = (uint32_t)minor;
		li->lo_rdevice = MKDEV(LOOP_MAJOR, minor);
		li->lo_sizelimit = loop_devs[minor].size_bytes;
		strncpy((char *)li->lo_file_name, loop_devs[minor].backing,
			LO_NAME_SIZE - 1);
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static int loop_cdev_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	int minor = (int)(uintptr_t)node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFBLK | 0660;
	s->st_size = 0;
	s->st_blksize = 512;
	s->st_blocks = (loff_t)(loop_devs[minor].size_bytes / 512);
	s->st_rdev = MKDEV(LOOP_MAJOR, minor);
	s->st_nlink = 1;
	return 0;
}

static ssize_t loop_cdev_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	int minor = (int)(uintptr_t)fp->f_inode->i_private;
	loop_device *ld = &loop_devices[minor];

	if (!ld->fp || !ld->fp->f_fop || !ld->fp->f_fop->read)
		return -ENXIO;

	return ld->fp->f_fop->read(ld->fp, buf, count, pos);
}

static ssize_t loop_cdev_write(file *fp, const void *buf, size_t count,
			       loff_t *pos)
{
	int minor = (int)(uintptr_t)fp->f_inode->i_private;
	loop_device *ld = &loop_devices[minor];

	if (!ld->fp || !ld->fp->f_fop || !ld->fp->f_fop->write)
		return -ENXIO;

	return ld->fp->f_fop->write(ld->fp, (void *)buf, count, pos);
}

static loff_t loop_cdev_llseek(file *fp, loff_t offset, int whence)
{
	int minor = (int)(uintptr_t)fp->f_inode->i_private;
	loff_t dev_size = (loff_t)loop_devs[minor].size_bytes;
	loff_t new_pos;

	if (!loop_devices[minor].fp)
		return -ENXIO;

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

static int loop_cdev_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations loop_cdev_fops = {
	.getattr = loop_cdev_getattr,
	.ioctl = loop_ioctl,
	.read = loop_cdev_read,
	.write = loop_cdev_write,
	.llseek = loop_cdev_llseek,
	.release = loop_cdev_release,
};

static file *loop_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	int minor = (int)MINOR(rdev);
	inode *node;
	file *fp;

	if (minor < 0 || minor >= LOOP_MAX_DEVS)
		return NULL;

	node = zalloc(sizeof(*node));
	node->i_mode = S_IFBLK | 0660;
	node->i_private = (void *)(uintptr_t)minor;

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &loop_cdev_fops;
	return fp;
}

/* ── Boot registration ──────────────────────────────────────────────────────── */

static void loop_dev_register(super_block *dev_sb)
{
	int i;
	char path[16];

	printk("dev: registered /dev/loop[0-%d]\n", LOOP_MAX_DEVS - 1);

	cdev_register(S_IFBLK, LOOP_MAJOR, 0, LOOP_MAX_DEVS, loop_cdev_open);

	for (i = 0; i < LOOP_MAX_DEVS; i++) {
		sprintf(loop_devs[i].name, "loop%d", i);
		blockdev_register(loop_devs[i].name, 7, i, 0, 0);
		sprintf(path, "/loop%d", i);
		vfs_mknod(dev_sb, path, S_IFBLK | 0660, MKDEV(LOOP_MAJOR, i));
	}
}

DEV_INIT(loop_dev_register);
