#include <dev/dev.h>
#include <errno.h>
#include <lib/command.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <fs/vfs.h>
#include <device/mouse.h>
#include <device/ps2.h>
#include <device/time.h>
#include <lib/klib.h>
#include <macro.h>
#include <unistd.h>
#include <dev/devnums.h>

#include "tty_ldisc.h"

static int mouse_file_nonblock(file *fp)
{
	return (fp->f_flag & O_NONBLOCK) != 0;
}

static ssize_t mouse_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	(void)pos;

	return ps2mouse_read(buf, size, mouse_file_nonblock(fp));
}

static ssize_t mouse_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	(void)fp;
	(void)pos;

	return ps2mouse_write(buf, size);
}

static unsigned mouse_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	return ps2mouse_poll(events, pt);
}

static int mouse_getattr(file *fp, struct stat *s)
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

static int mouse_ioctl_tcsbrk(void *context __attribute__((unused)),
			      unsigned cmd __attribute__((unused)),
			      void *buf __attribute__((unused)))
{
	/* PS/2 probe code expects tty-style flow-control ioctls to exist. */
	return 0;
}

static int mouse_ioctl_tcflsh(void *context __attribute__((unused)),
			      unsigned cmd __attribute__((unused)),
			      void *buf __attribute__((unused)))
{
	int sel = (int)(uintptr_t)buf;

	if (sel != TCIFLUSH && sel != TCOFLUSH && sel != TCIOFLUSH)
		sel = TCIOFLUSH;
	if (sel == TCIFLUSH || sel == TCIOFLUSH)
		ps2mouse_flush();
	return 0;
}

static int mouse_ioctl_tcgets(void *context __attribute__((unused)),
			      unsigned cmd __attribute__((unused)),
			      void *buf __attribute__((unused)))
{
	memcpy(buf, &tty_default_termios, sizeof(struct termios));
	return 0;
}

static int mouse_ioctl_tcsets(void *context __attribute__((unused)),
			      unsigned cmd __attribute__((unused)),
			      void *buf __attribute__((unused)))
{
	return 0;
}

static int mouse_ioctl_tcgeta(void *context __attribute__((unused)),
			      unsigned cmd __attribute__((unused)),
			      void *buf __attribute__((unused)))
{
	struct termio *t = (struct termio *)buf;

	memset(t, 0, sizeof(*t));
	t->c_iflag = (unsigned short)tty_default_termios.c_iflag;
	t->c_oflag = (unsigned short)tty_default_termios.c_oflag;
	t->c_cflag = (unsigned short)tty_default_termios.c_cflag;
	t->c_lflag = (unsigned short)tty_default_termios.c_lflag;
	t->c_line = tty_default_termios.c_line;
	memcpy(t->c_cc, tty_default_termios.c_cc, NCC);
	return 0;
}

static int mouse_ioctl_tcseta(void *context __attribute__((unused)),
			      unsigned cmd __attribute__((unused)),
			      void *buf __attribute__((unused)))
{
	return 0;
}

static int mouse_ioctl_tiocgwinsz(void *context __attribute__((unused)),
				  unsigned cmd __attribute__((unused)),
				  void *buf __attribute__((unused)))
{
	struct winsize *ws = (struct winsize *)buf;

	memset(ws, 0, sizeof(*ws));
	return 0;
}

static int mouse_ioctl_tiocswinsz(void *context __attribute__((unused)),
				  unsigned cmd __attribute__((unused)),
				  void *buf __attribute__((unused)))
{
	return 0;
}

static int mouse_ioctl_tiocmget(void *context __attribute__((unused)),
				unsigned cmd __attribute__((unused)),
				void *buf __attribute__((unused)))
{
	*(int *)buf = 0;
	return 0;
}

static int mouse_ioctl_fionread(void *context __attribute__((unused)),
				unsigned cmd __attribute__((unused)),
				void *buf __attribute__((unused)))
{
	*(int *)buf = ps2mouse_fionread();
	return 0;
}

static const command_operation mouse_terminal_commands[256] = {
	[TCSBRK & 255] = { TCSBRK, mouse_ioctl_tcsbrk },
	[TCXONC & 255] = { TCXONC, mouse_ioctl_tcsbrk },
	[TCFLSH & 255] = { TCFLSH, mouse_ioctl_tcflsh },
	[TCGETS & 255] = { TCGETS, mouse_ioctl_tcgets },
	[TCSETS & 255] = { TCSETS, mouse_ioctl_tcsets },
	[TCSETSW & 255] = { TCSETSW, mouse_ioctl_tcsets },
	[TCSETSF & 255] = { TCSETSF, mouse_ioctl_tcsets },
	[TCGETA & 255] = { TCGETA, mouse_ioctl_tcgeta },
	[TCSETA & 255] = { TCSETA, mouse_ioctl_tcseta },
	[TCSETAW & 255] = { TCSETAW, mouse_ioctl_tcseta },
	[TCSETAF & 255] = { TCSETAF, mouse_ioctl_tcseta },
	[TIOCGWINSZ & 255] = { TIOCGWINSZ, mouse_ioctl_tiocgwinsz },
	[TIOCSWINSZ & 255] = { TIOCSWINSZ, mouse_ioctl_tiocswinsz },
	[TIOCMGET & 255] = { TIOCMGET, mouse_ioctl_tiocmget },
	[FIONREAD & 255] = { FIONREAD, mouse_ioctl_fionread },
};

static const command_operation *const mouse_command_groups[256] = {
	[(TCSBRK >> 8) & 255] = mouse_terminal_commands,
};

static int mouse_ioctl(file *fp, unsigned cmd, void *buf)
{
	(void)fp;

	return command_dispatch(mouse_command_groups, fp, cmd, buf, -ENOTTY);
}

static int mouse_release(file *fp)
{
	ps2mouse_unregister_file(fp);
	ps2mouse_reader_close();
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations mouse_fops = {
	.release = mouse_release,
	.getattr = mouse_getattr,
	.read = mouse_read,
	.write = mouse_write,
	.poll = mouse_poll,
	.ioctl = mouse_ioctl,
};

static file *mouse_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	(void)dev_sb;
	(void)flag;

	node->i_mode = S_IFCHR | 0666;
	node->i_private = (void *)(uintptr_t)rdev;

	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &mouse_fops;
	fp->f_mode = (unsigned)(flag & O_ACCMODE);
	fp->f_flag = (unsigned)flag;
	fp->f_owner = 0;
	fp->f_sigio = 0;
	ps2mouse_register_file(fp);
	ps2mouse_reader_open();
	return fp;
}

static void mouse_dev_register(super_block *dev_sb)
{
	const device_t *device = ps2_device(PS2_PORT_AUX);
	if (!device || !device->driver)
		return;
	printk("dev: registered /dev/input/mice\n");
	cdev_register_named(S_IFCHR, INPUT_MOUSE_MAJOR, INPUT_MOUSE_MINOR, 1,
			    "input", mouse_cdev_open);
	vfs_mkdir(dev_sb, "/input", 0755);
	vfs_mknod(dev_sb, "/input/mice", S_IFCHR | 0666,
		  MKDEV(INPUT_MOUSE_MAJOR, INPUT_MOUSE_MINOR));
}

DEV_INIT(mouse_dev_register);
