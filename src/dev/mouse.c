#include <dev/dev.h>
#include <errno.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <fs/vfs.h>
#include <hw/time.h>
#include <lib/cyclebuf.h>
#include <lib/klib.h>
#include <macro.h>
#include <unistd.h>

#include "tty_ldisc.h"

/* /dev/input/mice — Linux-compatible aggregate PS/2 mouse node */
#define INPUT_MOUSE_MAJOR 13
#define INPUT_MOUSE_MINOR 63

static cy_buf *mouse_rxbuf;
static int mouse_expect_param;

#define PS2_ACK 0xfa
#define PS2_BAT_OK 0xaa
#define PS2_ID_STANDARD 0x00

static void mouse_queue_bytes(const unsigned char *buf, unsigned len)
{
	if (!mouse_rxbuf || !buf || len == 0)
		return;
	cyb_putbuf(mouse_rxbuf, (unsigned char *)buf, len, 0, 0);
}

static void mouse_queue_byte(unsigned char b)
{
	mouse_queue_bytes(&b, 1);
}

static void mouse_queue_idle_packet(void)
{
	static const unsigned char idle_packet[] = {
		0x08, /* sync bit set, no buttons */
		0x00,
		0x00,
		0x00, /* wheel delta for IMPS/2-style readers */
	};

	mouse_queue_bytes(idle_packet, sizeof(idle_packet));
}

static int mouse_file_nonblock(file *fp)
{
	return (fp->f_flag & O_NONBLOCK) != 0;
}

static ssize_t mouse_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	(void)pos;

	if (!buf || size < 1)
		return 0;
	if (!mouse_rxbuf)
		return -EIO;

	int ret =
		cyb_getbuf(mouse_rxbuf, buf, (int)size, !mouse_file_nonblock(fp), 1);
	if (ret < 0)
		return -EINTR;
	if (ret == 0 && mouse_file_nonblock(fp))
		return -EAGAIN;
	return ret;
}

static ssize_t mouse_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	const unsigned char *src = buf;
	size_t i;

	(void)fp;
	(void)pos;

	if (!buf || size < 1)
		return 0;

	for (i = 0; i < size; i++) {
		unsigned char cmd = src[i];

		if (mouse_expect_param) {
			mouse_expect_param = 0;
			mouse_queue_byte(PS2_ACK);
			continue;
		}

		switch (cmd) {
		case 0xff: {
			static const unsigned char reset_reply[] = {
				PS2_ACK,
				PS2_BAT_OK,
				PS2_ID_STANDARD,
			};

			if (mouse_rxbuf)
				cyb_flush(mouse_rxbuf);
			mouse_queue_bytes(reset_reply, sizeof(reset_reply));
			break;
		}
		case 0xf2: {
			static const unsigned char id_reply[] = {
				PS2_ACK,
				PS2_ID_STANDARD,
			};

			mouse_queue_bytes(id_reply, sizeof(id_reply));
			break;
		}
		case 0xeb: {
			static const unsigned char poll_reply[] = {
				PS2_ACK,
				0x08, /* always-present sync bit, no buttons */
				0x00,
				0x00,
			};

			mouse_queue_bytes(poll_reply, sizeof(poll_reply));
			break;
		}
		case 0xf3: /* set sample rate */
		case 0xe8: /* set resolution */
			mouse_queue_byte(PS2_ACK);
			mouse_expect_param = 1;
			break;
		case 0xe6: /* scaling 1:1 */
		case 0xe7: /* scaling 2:1 */
		case 0xea: /* stream mode */
		case 0xf0: /* remote mode */
		case 0xf5: /* disable data reporting */
		case 0xf6: /* set defaults */
			mouse_queue_byte(PS2_ACK);
			break;
		case 0xf4: /* enable data reporting */
			mouse_queue_byte(PS2_ACK);
			mouse_queue_idle_packet();
			break;
		default:
			/*
			 * Be permissive for probe traffic we don't emulate in
			 * detail yet; an ACK keeps X moving forward.
			 */
			mouse_queue_byte(PS2_ACK);
			break;
		}
	}

	return (ssize_t)size;
}

static unsigned mouse_poll(file *fp, unsigned events, poll_table *pt)
{
	unsigned ready = 0;

	(void)fp;
	if ((events & FS_POLL_READ) && mouse_rxbuf && !cyb_isempty(mouse_rxbuf))
		ready |= FS_POLL_READ;
	if (events & FS_POLL_WRITE)
		ready |= FS_POLL_WRITE;
	if (!ready && pt && (events & FS_POLL_READ) && mouse_rxbuf)
		cyb_poll_read(mouse_rxbuf, pt);
	return ready;
}

static int mouse_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = (unsigned)(uintptr_t)node->i_private;
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_nlink = 1;
	return 0;
}

static int mouse_ioctl(file *fp, unsigned cmd, void *buf)
{
	(void)fp;

	switch (cmd) {
	case TCSBRK:
	case TCXONC:
		/* PS/2 probe code expects tty-style flow-control ioctls to exist. */
		return 0;
	case TCFLSH: {
		int sel = (int)(uintptr_t)buf;

		if (sel != TCIFLUSH && sel != TCOFLUSH && sel != TCIOFLUSH)
			sel = TCIOFLUSH;
		if ((sel == TCIFLUSH || sel == TCIOFLUSH) && mouse_rxbuf)
			cyb_flush(mouse_rxbuf);
		return 0;
	}
	case TCGETS:
		memcpy(buf, &tty_default_termios, sizeof(struct termios));
		return 0;
	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		return 0;
	case TCGETA: {
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
	case TCSETA:
	case TCSETAW:
	case TCSETAF:
		return 0;
	case TIOCGWINSZ: {
		struct winsize *ws = (struct winsize *)buf;

		memset(ws, 0, sizeof(*ws));
		return 0;
	}
	case TIOCSWINSZ:
		return 0;
	case TIOCMGET:
		*(int *)buf = 0;
		return 0;
	case FIONREAD:
		*(int *)buf = mouse_rxbuf ? cyb_get_buf_len(mouse_rxbuf) : 0;
		return 0;
	default:
		return -ENOTTY;
	}
}

static int mouse_release(file *fp)
{
	if (mouse_rxbuf)
		cyb_reader_close(mouse_rxbuf);
	mouse_expect_param = 0;
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations mouse_iops = {
	.getattr = mouse_getattr,
};

static const file_operations mouse_fops = {
	.release = mouse_release,
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
	node->i_op = &mouse_iops;

	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &mouse_fops;
	fp->f_mode = (unsigned)(flag & O_ACCMODE);
	fp->f_flag = (unsigned)flag;
	if (TestControl.verbos)
		klog("mouse_open(rdev=%x, flag=%x)\n", rdev, flag);
	if (mouse_rxbuf)
		cyb_reader_open(mouse_rxbuf);
	mouse_queue_idle_packet();
	return fp;
}

static void mouse_dev_register(super_block *dev_sb)
{
	mouse_rxbuf = cyb_create_named(1);
	if (mouse_rxbuf)
		cyb_writer_open(mouse_rxbuf);
	printk("dev: registered /dev/input/mice\n");
	cdev_register(S_IFCHR, INPUT_MOUSE_MAJOR, INPUT_MOUSE_MINOR, 1,
		      mouse_cdev_open);
	vfs_mkdir(dev_sb, "/input", 0755);
	vfs_mknod(dev_sb, "/input/mice", S_IFCHR | 0666,
		  MKDEV(INPUT_MOUSE_MAJOR, INPUT_MOUSE_MINOR));
}

DEV_INIT(mouse_dev_register);
