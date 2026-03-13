
#include <lib/timer.h>
#include <lib/klib.h>
#include <fs/fs.h>
#include <fs/mount.h>
#include <fs/ioctl.h>
#include <ps/ps.h>
#include <hw/time.h>
#include <hw/keyboard.h>
#include <hw/tty.h>
#include <macro.h>
#include <unistd.h>

#define CANON_BUF_SIZE 256
static char canon_buf[CANON_BUF_SIZE];
static int canon_len;

/* Apply c_iflag input translations.  Returns the translated character,
 * or -1 if the character should be discarded (IGNCR). */
static int input_translate(unsigned char c, tcflag_t iflag)
{
	if (iflag & ISTRIP)
		c &= 0x7f;
	if (c == '\r') {
		if (iflag & IGNCR)
			return -1;
		if (iflag & ICRNL)
			return '\n';
	} else if (c == '\n' && (iflag & INLCR)) {
		return '\r';
	}
	if (iflag & IUCLC)
		c = (unsigned char)tolower(c);
	return (int)c;
}

/* Returns 1 if c is a signal character (ISIG) and should be discarded. */
static int isig_check(unsigned char c, const struct termios *tc)
{
	if (!(tc->c_lflag & ISIG))
		return 0;
	if ((tc->c_cc[VINTR] && c == tc->c_cc[VINTR]) || /* TODO: SIGINT  */
	    (tc->c_cc[VQUIT] && c == tc->c_cc[VQUIT]) || /* TODO: SIGQUIT */
	    (tc->c_cc[VSUSP] && c == tc->c_cc[VSUSP])) /* TODO: SIGTSTP */
		return 1;
	return 0;
}

/* Erase the last character from the canonical buffer (VERASE). */
static void canon_erase(const struct termios *tc)
{
	if (canon_len == 0)
		return;
	canon_len--;
	if (!(tc->c_lflag & ECHO))
		return;
	if (tc->c_lflag & ECHOE)
		tty_write("\b \b", 3);
	else
		tty_write((const char *)&tc->c_cc[VERASE], 1);
}

/* Erase the entire canonical buffer (VKILL). */
static void canon_kill(const struct termios *tc)
{
	if (tc->c_lflag & ECHO) {
		if (tc->c_lflag & ECHOK) {
			int i;
			for (i = 0; i < canon_len; i++)
				tty_write("\b \b", 3);
		} else {
			tty_write((const char *)&tc->c_cc[VKILL], 1);
			tty_write("\n", 1);
		}
	}
	canon_len = 0;
}

/* Append one character to the canonical buffer and echo it. */
static void canon_append(unsigned char c, const struct termios *tc)
{
	if (canon_len >= CANON_BUF_SIZE - 1)
		return;
	canon_buf[canon_len++] = (char)c;
	if (tc->c_lflag & ECHO)
		tty_write(&canon_buf[canon_len - 1], 1);
	else if (c == '\n' && (tc->c_lflag & ECHONL))
		tty_write("\n", 1);
}

static int is_eol(unsigned char c, const struct termios *tc)
{
	return c == '\n' || (tc->c_cc[VEOL] && c == tc->c_cc[VEOL]) ||
	       (tc->c_cc[VEOL2] && c == tc->c_cc[VEOL2]);
}

/* Read characters into canon_buf until a complete line is ready.
 * Returns 1 when a line is available, 0 on EOF (VEOF on empty buffer). */
static int canon_readline(const struct termios *tc)
{
	while (1) {
		int ch = input_translate(kb_buf_get(), tc->c_iflag);
		if (ch < 0)
			continue;

		unsigned char c = (unsigned char)ch;

		if (isig_check(c, tc))
			continue;
		if (tc->c_cc[VERASE] && c == tc->c_cc[VERASE]) {
			canon_erase(tc);
			continue;
		}
		if (tc->c_cc[VKILL] && c == tc->c_cc[VKILL]) {
			canon_kill(tc);
			continue;
		}
		if (tc->c_cc[VEOF] && c == tc->c_cc[VEOF])
			return canon_len > 0; /* 0 = true EOF */

		canon_append(c, tc);

		if (is_eol(c, tc))
			return 1;
	}
}

/* Copy up to size bytes from canon_buf to dst, consuming what was copied. */
static ssize_t canon_drain(char *dst, size_t size)
{
	ssize_t n =
		(ssize_t)(size < (size_t)canon_len ? size : (size_t)canon_len);
	memcpy(dst, canon_buf, (unsigned)n);
	canon_len -= (int)n;
	if (canon_len > 0)
		memmove(canon_buf, canon_buf + n, (unsigned)canon_len);
	return n;
}

/* Raw mode read: block until VMIN chars are available, then drain up to size. */
static ssize_t raw_read(char *dst, size_t size, const struct termios *tc)
{
	unsigned vmin = tc->c_cc[VMIN];
	ssize_t n = 0;

	while ((size_t)n < size) {
		if ((unsigned)n >= vmin && !kb_can_read())
			break;
		int ch = input_translate(kb_buf_get(), tc->c_iflag);
		if (ch < 0)
			continue;
		dst[n++] = (char)ch;
		if (tc->c_lflag & ECHO)
			tty_write(&dst[n - 1], 1);
	}
	return n;
}

static ssize_t kb_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	struct termios *tc = tty_gettermios();

	if (size < 1 || !buf)
		return 0;

	if (tc->c_lflag & ICANON) {
		if (canon_len == 0 && !canon_readline(tc))
			return 0;
		return canon_drain(buf, size);
	}
	return raw_read(buf, size, tc);
}

static int kb_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_WRITE || type == FS_POLL_EXCEPT)
		return -1;
	return 0;
}

static int kb_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0;
	s->st_rdev = 5;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_size = PAGE_SIZE;
	return 0;
}

static const inode_operations kb_iops = {
	.getattr = kb_getattr,
};

static const file_operations kb_fops = {
	.read = kb_read,
	.poll = kb_poll,
	.ioctl = tty_ioctl,
};

static file *kb_open_root(super_block *sb)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = (S_IFCHR | S_IRUSR | S_IRGRP | S_IROTH);
	node->i_op = &kb_iops;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &kb_fops;
	return fp;
}

static super_operations kb_sops = {
	.open_root = kb_open_root,
};

static void kbchar_init()
{
	task_struct *cur = CURRENT_TASK();
	vfs_mount(cur->root, "/dev/kb", sget(&kb_sops));
}

KERNEL_INIT(4, kbchar_init);
