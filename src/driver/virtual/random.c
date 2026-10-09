#include <fs/entries.h>
#include <driver/driver.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <macro.h>
#include <device/devnode.h>
#include <unistd.h>
#include <device/devnums.h>

static ssize_t random_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	unsigned char bytes[64];
	size_t copied = 0;
	(void)fp;
	(void)pos;
	while (copied < size) {
		unsigned count = size - copied < sizeof(bytes) ? size - copied :
								 sizeof(bytes);
		kernel_random_bytes(bytes, count);
		memcpy((char *)buf + copied, bytes, count);
		copied += count;
	}
	return (ssize_t)copied;
}

static ssize_t random_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	/* Mix the current clock into fallback state. */
	kernel_random_mix(time_now_us());
	return (ssize_t)size;
}

static unsigned random_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return events & (FS_POLL_READ | FS_POLL_WRITE);
}

static int random_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;

	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = (unsigned)(uintptr_t)node->i_private;
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_nlink = 1;
	return 0;
}

static int random_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations random_fops = {
	.release = random_release,
	.getattr = random_getattr,
	.read = random_read,
	.write = random_write,
	.poll = random_poll,
};

static file *random_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_private = (void *)(uintptr_t)rdev;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &random_fops;
	return fp;
}

static void random_dev_register(void)
{

	
	vfs_entry_device(devfs_entries(), "/random", S_IFCHR | 0666, MKDEV(RANDOM_MAJOR, RANDOM_MINOR), "mem", random_cdev_open);

	
	vfs_entry_device(devfs_entries(), "/urandom", S_IFCHR | 0666, MKDEV(RANDOM_MAJOR, URANDOM_MINOR), "mem", random_cdev_open);
}

static int random_dev_register_probe(void)
{
	random_dev_register();
	return 0;
}

static driver_t random_dev_register_driver = {
	.name = "random",
	.bus = DEVICE_BUS_VIRTUAL,
	.virtual_id = VDEV_RANDOM,
	.probe_virtual = random_dev_register_probe,
};
DRIVER_REGISTER(random_dev_register_driver);
