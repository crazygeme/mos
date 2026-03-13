
#include <ps/ps.h>
#include <fs/mount.h>
#include <fs/fs.h>
#include <fs/ioctl.h>
#include <lib/klib.h>
#include <unistd.h>
#include <hw/time.h>
#include <hw/tty.h>
#include <macro.h>
#include <ext4.h>

/* Returns 1 if the cursor is currently at column 0. */
static int at_column_zero(void)
{
	return tty_get_cursor() % (int)TTY_MAX_COL == 0;
}

/* Apply c_oflag output processing and write one character to the TTY.
 * If OPOST is clear, the character is written raw. */
static void output_char(unsigned char c, const struct termios *tc)
{
	if (!(tc->c_oflag & OPOST)) {
		tty_write((const char *)&c, 1);
		return;
	}

	if (tc->c_oflag & OLCUC)
		c = (unsigned char)toupper(c);

	if (c == '\n') {
		if (tc->c_oflag & ONLCR) {
			tty_write("\r\n", 2);
			return;
		}
	} else if (c == '\r') {
		if (tc->c_oflag & OCRNL) {
			tty_write("\n", 1);
			return;
		}
		if ((tc->c_oflag & ONOCR) && at_column_zero())
			return;
	}

	tty_write((const char *)&c, 1);
}

static ssize_t console_write(file *fp, const void *buf, size_t size,
			     loff_t *pos)
{
	const unsigned char *src = buf;
	struct termios *tc = tty_gettermios();
	size_t i;

	if (size < 1 || !buf)
		return 0;

	for (i = 0; i < size; i++)
		output_char(src[i], tc);

	return (ssize_t)size;
}

static ssize_t console_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	memset(buf, '?', size);
	return (ssize_t)size;
}

static loff_t console_llseek(file *fp, loff_t offset, int whence)
{
	switch (whence) {
	case SEEK_SET:
		tty_set_cursor(offset);
		break;
	case SEEK_CUR:
		tty_set_cursor(offset + tty_get_cursor());
		break;
	case SEEK_END:
		tty_set_cursor(TTY_MAX_CHARS - offset);
		break;
	default:
		break;
	}
	tty_flush_cursor();
	fp->f_pos = tty_get_cursor();
	return fp->f_pos;
}

static int console_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT || type == FS_POLL_READ)
		return -1;
	/* can always write */
	return 0;
}

static int console_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0xb;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_nlink = 1;
	s->st_rdev = 8004;
	s->st_size = TTY_MAX_CHARS - tty_get_cursor();
	return 0;
}

static const inode_operations console_iops = {
	.getattr = console_getattr,
};

static const file_operations console_fops = {
	.read = console_read,
	.write = console_write,
	.llseek = console_llseek,
	.poll = console_poll,
	.ioctl = tty_ioctl,
};

static file *console_open_root(super_block *sb)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR);
	node->i_op = &console_iops;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &console_fops;
	return fp;
}

static super_operations tty_sops = {
	.open_root = console_open_root,
};

static void console_init(void)
{
	task_struct *cur = CURRENT_TASK();
	vfs_mount(cur->root, "/dev/tty", sget(&tty_sops));
}

KERNEL_INIT(4, console_init);
