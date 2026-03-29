#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <dev/dev.h>
#include <unistd.h>

/* /dev/random  — major 1, minor 8 (Linux-compatible) */
/* /dev/urandom — major 1, minor 9 (Linux-compatible) */
#define RANDOM_MAJOR 1
#define RANDOM_MINOR 8
#define URANDOM_MINOR 9

static int random_seeded = 0;

static void random_ensure_seeded(void)
{
	if (!random_seeded) {
		srand((unsigned)time_now_ms());
		random_seeded = 1;
	}
}

static ssize_t random_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	random_ensure_seeded();
	unsigned char *p = buf;
	for (size_t i = 0; i < size; i++)
		p[i] = (unsigned char)(rand() & 0xff);
	return (ssize_t)size;
}

static ssize_t random_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	/* Treat writes as entropy feed: re-seed with the current time */
	srand((unsigned)time_now_ms());
	return (ssize_t)size;
}

static int random_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT)
		return -1;
	return 0;
}

static int random_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = (unsigned)(uintptr_t)node->i_private;
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_now_ms();
	s->st_ctime = time_now_ms();
	s->st_nlink = 1;
	return 0;
}

static int random_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations random_iops = {
	.getattr = random_getattr,
};

static const file_operations random_fops = {
	.release = random_release,
	.read = random_read,
	.write = random_write,
	.is_ready = random_poll,
};

static file *random_cdev_open(unsigned rdev, int flag)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_private = (void *)(uintptr_t)rdev;
	node->i_op = &random_iops;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &random_fops;
	return fp;
}

static void random_dev_register(super_block *dev_sb)
{
	cdev_register(S_IFCHR, RANDOM_MAJOR, RANDOM_MINOR, 1, random_cdev_open);
	vfs_mknod(dev_sb, "/random", S_IFCHR | 0666,
		  MKDEV(RANDOM_MAJOR, RANDOM_MINOR));

	cdev_register(S_IFCHR, RANDOM_MAJOR, URANDOM_MINOR, 1,
		      random_cdev_open);
	vfs_mknod(dev_sb, "/urandom", S_IFCHR | 0666,
		  MKDEV(RANDOM_MAJOR, URANDOM_MINOR));
}

DEV_INIT(random_dev_register);
