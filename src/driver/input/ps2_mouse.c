#include <fs/entries.h>
#include <errno.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <driver/driver.h>
#include <lib/cyclebuf.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <macro.h>
#include <ps/ps.h>
#include <ps/signal.h>
#include <device/devnode.h>
#include <lib/command.h>
#include <fs/ioctl.h>
#include <fs/vfs.h>
#include <unistd.h>
#include <device/devnums.h>
#include "../tty/tty_ldisc.h"

static void mouse_dev_register(void);

#define PS2_MOUSE_ACK 0xFA
#define PS2_MOUSE_BAT_OK 0xAA
#define PS2_MOUSE_ID_STANDARD 0x00
#define PS2_MOUSE_ID_IMPS2 0x03

#define PS2_MOUSE_CMD_RESET 0xFF
#define PS2_MOUSE_CMD_RESEND 0xFE
#define PS2_MOUSE_CMD_SET_DEFAULTS 0xF6
#define PS2_MOUSE_CMD_DISABLE_REPORTING 0xF5
#define PS2_MOUSE_CMD_ENABLE_REPORTING 0xF4
#define PS2_MOUSE_CMD_SET_SAMPLE_RATE 0xF3
#define PS2_MOUSE_CMD_GET_ID 0xF2
#define PS2_MOUSE_CMD_SET_REMOTE_MODE 0xF0
#define PS2_MOUSE_CMD_READ_DATA 0xEB
#define PS2_MOUSE_CMD_SET_STREAM_MODE 0xEA
#define PS2_MOUSE_CMD_STATUS_REQUEST 0xE9
#define PS2_MOUSE_CMD_SET_RESOLUTION 0xE8
#define PS2_MOUSE_CMD_SET_SCALING21 0xE7
#define PS2_MOUSE_CMD_SET_SCALING11 0xE6

static cy_buf *mouse_rxbuf;
static mutex_t mouse_cmd_lock;
static spinlock_t mouse_state_lock;
static int mouse_present;
#define PS2MOUSE_MAX_FILES 8
static file *mouse_files[PS2MOUSE_MAX_FILES];

static unsigned char mouse_packet[4];
static unsigned mouse_packet_idx;
static unsigned mouse_packet_len = 3;
static unsigned char mouse_last_packet[4] = { 0x08, 0x00, 0x00, 0x00 };
static unsigned mouse_last_packet_len = 3;

static int mouse_expect_param;
static unsigned char mouse_expect_param_cmd;
static unsigned char mouse_last_sample[3];

static void mouse_queue_bytes(const unsigned char *buf, unsigned len)
{
	int irq;
	int owners[PS2MOUSE_MAX_FILES];
	int sigs[PS2MOUSE_MAX_FILES];
	unsigned nnotify = 0;
	unsigned i;

	if (!mouse_rxbuf || !buf || len == 0)
		return;
	if (cyb_put_record(mouse_rxbuf, buf, len) != (int)len)
		return;

	spinlock_lock(&mouse_state_lock, &irq);
	for (i = 0; i < PS2MOUSE_MAX_FILES; i++) {
		file *fp = mouse_files[i];

		if (!fp || !(fp->f_flag & FASYNC) || !fp->f_owner)
			continue;
		owners[nnotify] = fp->f_owner;
		sigs[nnotify] = fp->f_sigio > 0 ? fp->f_sigio : SIGIO;
		nnotify++;
	}
	spinlock_unlock(&mouse_state_lock, irq);

	for (i = 0; i < nnotify; i++) {
		ps_send_signal_owner(owners[i], sigs[i]);
	}
}

static void mouse_queue_byte(unsigned char b)
{
	mouse_queue_bytes(&b, 1);
}

static void mouse_update_packet_mode_locked(unsigned char id)
{
	if (id == PS2_MOUSE_ID_IMPS2) {
		mouse_packet_len = 4;
		mouse_last_packet_len = 4;
		mouse_last_packet[3] = 0x00;
	} else {
		mouse_packet_len = 3;
		mouse_last_packet_len = 3;
	}
	mouse_packet_idx = 0;
}

static void mouse_reset_runtime_state(void)
{
	int irq;

	spinlock_lock(&mouse_state_lock, &irq);
	mouse_expect_param = 0;
	mouse_expect_param_cmd = 0;
	mouse_packet_idx = 0;
	mouse_last_sample[0] = 0;
	mouse_last_sample[1] = 0;
	mouse_last_sample[2] = 0;
	mouse_update_packet_mode_locked(PS2_MOUSE_ID_STANDARD);
	spinlock_unlock(&mouse_state_lock, irq);
}

static int ps2mouse_send_cmd(unsigned char cmd)
{
	return ps2_write_byte(PS2_PORT_AUX, cmd);
}

static int ps2mouse_send_cmd_expect_ack(unsigned char cmd)
{
	unsigned char reply;

	if (ps2mouse_send_cmd(cmd) < 0)
		return -ETIMEDOUT;
	if (ps2_read_reply(PS2_PORT_AUX, &reply, PS2_REPLY_SPINS) < 0)
		return -ETIMEDOUT;
	return reply == PS2_MOUSE_ACK ? 0 : -EIO;
}

static void mouse_track_sample_rate_locked(unsigned char rate)
{
	mouse_last_sample[0] = mouse_last_sample[1];
	mouse_last_sample[1] = mouse_last_sample[2];
	mouse_last_sample[2] = rate;

	if (mouse_last_sample[0] == 200 && mouse_last_sample[1] == 100 &&
	    mouse_last_sample[2] == 80)
		mouse_update_packet_mode_locked(PS2_MOUSE_ID_IMPS2);
}

static void ps2mouse_process_byte(unsigned char data)
{
	unsigned char packet[4];
	unsigned packet_len = 0;
	int irq;

	spinlock_lock(&mouse_state_lock, &irq);

	if (mouse_packet_idx == 0 && (data & 0x08) == 0) {
		spinlock_unlock(&mouse_state_lock, irq);
		return;
	}

	mouse_packet[mouse_packet_idx++] = data;
	if (mouse_packet_idx == mouse_packet_len) {
		unsigned i;

		for (i = 0; i < mouse_packet_len; i++)
			packet[i] = mouse_packet[i];
		packet_len = mouse_packet_len;
		for (i = 0; i < packet_len; i++)
			mouse_last_packet[i] = packet[i];
		mouse_last_packet_len = packet_len;
		mouse_packet_idx = 0;
	}

	spinlock_unlock(&mouse_state_lock, irq);

	if (packet_len)
		mouse_queue_bytes(packet, packet_len);
}

static int ps2mouse_probe(unsigned port)
{
	(void)port;
	mouse_rxbuf = cyb_create_named(1);
	if (!mouse_rxbuf)
		return -ENOMEM;
	cyb_writer_open(mouse_rxbuf);
	mutex_init(&mouse_cmd_lock);
	spinlock_init(&mouse_state_lock);
	mouse_reset_runtime_state();
	ps2_command_begin(PS2_PORT_AUX);
	if (ps2mouse_send_cmd_expect_ack(PS2_MOUSE_CMD_SET_DEFAULTS) < 0 ||
	    ps2mouse_send_cmd_expect_ack(PS2_MOUSE_CMD_ENABLE_REPORTING) < 0) {
		ps2_command_end(PS2_PORT_AUX);
		cyb_writer_close(mouse_rxbuf);
		cyb_destroy(mouse_rxbuf);
		mouse_rxbuf = NULL;
		printk("mouse: PS/2 mouse not responding\n");
		return -ENODEV;
	}
	mouse_present = 1;
	ps2_command_end(PS2_PORT_AUX);
	mouse_dev_register();
	return 0;
}

static void ps2mouse_reader_open(void)
{
	if (mouse_rxbuf)
		cyb_reader_open(mouse_rxbuf);
}

static void ps2mouse_reader_close(void)
{
	if (mouse_rxbuf)
		cyb_reader_close(mouse_rxbuf);
}

static void ps2mouse_register_file(file *fp)
{
	unsigned i;
	int irq;

	if (!fp)
		return;

	spinlock_lock(&mouse_state_lock, &irq);
	for (i = 0; i < PS2MOUSE_MAX_FILES; i++) {
		if (mouse_files[i] == fp) {
			spinlock_unlock(&mouse_state_lock, irq);
			return;
		}
	}
	for (i = 0; i < PS2MOUSE_MAX_FILES; i++) {
		if (!mouse_files[i]) {
			mouse_files[i] = fp;
			break;
		}
	}
	spinlock_unlock(&mouse_state_lock, irq);
}

static void ps2mouse_unregister_file(file *fp)
{
	unsigned i;
	int irq;

	if (!fp)
		return;

	spinlock_lock(&mouse_state_lock, &irq);
	for (i = 0; i < PS2MOUSE_MAX_FILES; i++) {
		if (mouse_files[i] == fp) {
			mouse_files[i] = NULL;
			break;
		}
	}
	spinlock_unlock(&mouse_state_lock, irq);
}

static ssize_t ps2mouse_read(void *buf, size_t size, int nonblock)
{
	int ret;

	if (!buf || size < 1)
		return 0;
	if (!mouse_present || !mouse_rxbuf)
		return -EIO;

	ret = cyb_getbuf(mouse_rxbuf, buf, (int)size, !nonblock, 1);
	if (ret < 0)
		return -EINTR;
	if (ret == 0 && nonblock)
		return -EAGAIN;
	return ret;
}

static int ps2mouse_queue_reply(const unsigned char *reply, unsigned len)
{
	unsigned i;

	for (i = 0; i < len; i++)
		mouse_queue_byte(reply[i]);
	return 0;
}

static int ps2mouse_command_one(unsigned char byte, int *expect_param,
				unsigned char *expect_cmd)
{
	unsigned char reply[5];
	unsigned len = 0;
	int irq;

	if (*expect_param) {
		if (ps2_write_byte(PS2_PORT_AUX, byte) < 0)
			return -EIO;
		if (ps2_read_reply(PS2_PORT_AUX, &reply[len], PS2_REPLY_SPINS) <
		    0)
			return -EIO;
		len++;

		spinlock_lock(&mouse_state_lock, &irq);
		if (reply[0] == PS2_MOUSE_ACK) {
			if (*expect_cmd == PS2_MOUSE_CMD_SET_SAMPLE_RATE)
				mouse_track_sample_rate_locked(byte);
		}
		*expect_param = 0;
		mouse_expect_param = 0;
		*expect_cmd = 0;
		mouse_expect_param_cmd = 0;
		spinlock_unlock(&mouse_state_lock, irq);
		return ps2mouse_queue_reply(reply, len);
	}

	if (ps2_write_byte(PS2_PORT_AUX, byte) < 0)
		return -EIO;
	if (ps2_read_reply(PS2_PORT_AUX, &reply[len], PS2_REPLY_SPINS) < 0)
		return -EIO;
	len++;

	switch (byte) {
	case PS2_MOUSE_CMD_RESET:
		if (ps2_read_reply(PS2_PORT_AUX, &reply[len], PS2_REPLY_SPINS) <
		    0)
			return -EIO;
		len++;
		if (ps2_read_reply(PS2_PORT_AUX, &reply[len], PS2_REPLY_SPINS) <
		    0)
			return -EIO;
		len++;

		spinlock_lock(&mouse_state_lock, &irq);
		mouse_last_sample[0] = 0;
		mouse_last_sample[1] = 0;
		mouse_last_sample[2] = 0;
		mouse_update_packet_mode_locked(reply[len - 1]);
		*expect_param = 0;
		*expect_cmd = 0;
		mouse_expect_param = 0;
		mouse_expect_param_cmd = 0;
		spinlock_unlock(&mouse_state_lock, irq);
		break;
	case PS2_MOUSE_CMD_GET_ID:
		if (ps2_read_reply(PS2_PORT_AUX, &reply[len], PS2_REPLY_SPINS) <
		    0)
			return -EIO;
		len++;

		spinlock_lock(&mouse_state_lock, &irq);
		mouse_update_packet_mode_locked(reply[len - 1]);
		spinlock_unlock(&mouse_state_lock, irq);
		break;
	case PS2_MOUSE_CMD_READ_DATA: {
		unsigned packet_len;

		spinlock_lock(&mouse_state_lock, &irq);
		packet_len = mouse_packet_len;
		spinlock_unlock(&mouse_state_lock, irq);

		while (packet_len-- > 0) {
			if (ps2_read_reply(PS2_PORT_AUX, &reply[len],
					   PS2_REPLY_SPINS) < 0)
				return -EIO;
			len++;
		}
		break;
	}
	case PS2_MOUSE_CMD_STATUS_REQUEST:
		if (ps2_read_reply(PS2_PORT_AUX, &reply[len], PS2_REPLY_SPINS) <
		    0)
			return -EIO;
		len++;
		if (ps2_read_reply(PS2_PORT_AUX, &reply[len], PS2_REPLY_SPINS) <
		    0)
			return -EIO;
		len++;
		if (ps2_read_reply(PS2_PORT_AUX, &reply[len], PS2_REPLY_SPINS) <
		    0)
			return -EIO;
		len++;
		break;
	case PS2_MOUSE_CMD_SET_SAMPLE_RATE:
	case PS2_MOUSE_CMD_SET_RESOLUTION:
		spinlock_lock(&mouse_state_lock, &irq);
		*expect_param = 1;
		*expect_cmd = byte;
		mouse_expect_param = 1;
		mouse_expect_param_cmd = byte;
		spinlock_unlock(&mouse_state_lock, irq);
		break;
	case PS2_MOUSE_CMD_SET_DEFAULTS:
		spinlock_lock(&mouse_state_lock, &irq);
		mouse_last_sample[0] = 0;
		mouse_last_sample[1] = 0;
		mouse_last_sample[2] = 0;
		mouse_update_packet_mode_locked(PS2_MOUSE_ID_STANDARD);
		mouse_expect_param = 0;
		mouse_expect_param_cmd = 0;
		spinlock_unlock(&mouse_state_lock, irq);
		break;
	default:
		break;
	}

	return ps2mouse_queue_reply(reply, len);
}

static ssize_t ps2mouse_write(const void *buf, size_t size)
{
	const unsigned char *src = buf;
	size_t i;
	int expect_param;
	unsigned char expect_cmd;

	if (!buf || size < 1)
		return 0;
	if (!mouse_present)
		return -EIO;

	mutex_lock(&mouse_cmd_lock);
	ps2_command_begin(PS2_PORT_AUX);

	expect_param = mouse_expect_param;
	expect_cmd = mouse_expect_param_cmd;
	for (i = 0; i < size; i++) {
		if (ps2mouse_command_one(src[i], &expect_param, &expect_cmd) <
		    0) {
			ps2_command_end(PS2_PORT_AUX);
			mutex_unlock(&mouse_cmd_lock);
			return -EIO;
		}
	}

	ps2_command_end(PS2_PORT_AUX);
	mutex_unlock(&mouse_cmd_lock);
	return (ssize_t)size;
}

static unsigned ps2mouse_poll(unsigned events, poll_table *pt)
{
	unsigned ready = 0;

	if ((events & FS_POLL_READ) && mouse_rxbuf && !cyb_isempty(mouse_rxbuf))
		ready |= FS_POLL_READ;
	if (events & FS_POLL_WRITE)
		ready |= FS_POLL_WRITE;
	if (pt && (events & FS_POLL_READ) && mouse_rxbuf)
		cyb_poll_read(mouse_rxbuf, pt);
	return ready;
}

static void ps2mouse_flush(void)
{
	if (mouse_rxbuf)
		cyb_flush(mouse_rxbuf);
}

static int ps2mouse_fionread(void)
{
	return mouse_rxbuf ? cyb_get_buf_len(mouse_rxbuf) : 0;
}

static driver_t ps2_mouse_driver = {
	.name = "ps2-mouse",
	.bus = DEVICE_BUS_PS2,
	.early = 1,
	.ps2_port = PS2_PORT_AUX,
	.probe_ps2 = ps2mouse_probe,
	.receive_ps2 = ps2mouse_process_byte,
};
DRIVER_REGISTER(ps2_mouse_driver);

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

static void mouse_dev_register(void)
{
	if (!mouse_present)
		return;

	vfs_entry_directory_path(devfs_entries(), "input", 0755);
	vfs_entry_device(devfs_entries(), "/input/mice", S_IFCHR | 0666,
			 MKDEV(INPUT_MOUSE_MAJOR, INPUT_MOUSE_MINOR), "input",
			 mouse_cdev_open);
}
