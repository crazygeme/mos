/* Publish the operational VirtIO GPU endpoints in /dev and /sys. */
#include <dev/dev.h>
#include <dev/virtgpu.h>
#include <driver/video/virtio_gpu.h>
#include <driver/drm/sysfs.h>
#include <lib/klib.h>
#include <errno.h>
#include <macro.h>

static int gpu_dir_stat(file *fp, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFDIR | 0555;
	st->st_ino = GPU_MAJOR;
	st->st_size = fp->f_inode->i_size;
	st->st_blksize = PAGE_SIZE;
	st->st_nlink = 2;
	return 0;
}

static ssize_t gpu_dir_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	unsigned length = fp->f_inode->i_size;
	if (*pos < 0)
		return -EINVAL;
	if ((uint64_t)*pos >= length)
		return 0;
	if (size > length - *pos)
		size = length - *pos;
	memcpy(buf, (char *)fp->f_inode->i_private + *pos, size);
	*pos += size;
	return size;
}

static int gpu_dir_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations gpu_dir_fops = {
	.read = gpu_dir_read,
	.getattr = gpu_dir_stat,
	.release = gpu_dir_release,
};

static file *gpu_dir_open(super_block *sb, int flags)
{
	file *fp = zalloc(sizeof(*fp));
	char *p, *begin;
	struct linux_dirent *dirp;
	(void)sb;
	(void)flags;
	if (!fp)
		return NULL;
	fp->f_inode = zalloc(sizeof(*fp->f_inode));
	if (!fp->f_inode) {
		free(fp);
		return NULL;
	}
	begin = p = zalloc(256);
	if (!p) {
		free(fp->f_inode);
		free(fp);
		return NULL;
	}
	FILL_ENTRY(".", GPU_MAJOR);
	FILL_ENTRY("..", DEV_INODE);
	FILL_ENTRY("card0", MKDEV(GPU_MAJOR, 0));
	FILL_ENTRY("renderD128", MKDEV(GPU_MAJOR, GPU_RENDER_MINOR));
	fp->f_inode->i_private = begin;
	fp->f_inode->i_size = p - begin;
	fp->f_inode->i_mode = S_IFDIR | 0555;
	fp->f_fop = &gpu_dir_fops;
	fp->f_count = 1;
	return fp;
}

static const super_operations gpu_dir_sops = { .open_root = gpu_dir_open };

static void gpu_register(super_block *sb)
{
	unsigned address;
	if (gpu_device_address(&address))
		return;
	if (drm_sysfs_register(address))
		printk("virtio_gpu: cannot register sys entries\n");
	cdev_register_named(S_IFCHR, GPU_MAJOR, 0, 1, "drm", gpu_open);
	cdev_register_named(S_IFCHR, GPU_MAJOR, GPU_RENDER_MINOR, 1, "drm",
			    gpu_open);
	{
		super_block *directory = sget(&gpu_dir_sops);
		vfs_mount(sb, "/dri", directory);
		vfs_mknod(directory, "/card0", S_IFCHR | 0666,
			  MKDEV(GPU_MAJOR, 0));
		vfs_mknod(directory, "/renderD128", S_IFCHR | 0666,
			  MKDEV(GPU_MAJOR, GPU_RENDER_MINOR));
	}
}
DEV_INIT(gpu_register);
