/*
 * src/dev/rtc.c — /dev/rtc real-time clock device.
 *
 * Exposes the MC146818A-compatible CMOS RTC via:
 *   ioctl(fd, RTC_RD_TIME, &rtc_time)  — read current wall-clock time
 *
 * Character device, major 10, minor 135 (Linux-compatible).
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/port.h>
#include <device/time_internal.h>
#include <dev/dev.h>
#include <macro.h>
#include <errno.h>
#include <lib/command.h>
#include <unistd.h>
#include <dev/devnums.h>

/* ── Linux-compatible RTC ioctls ─────────────────────────────────────────── */

#define RTC_RD_TIME 0x80247009 /* read time */
#define RTC_UIE_ON 0x7003 /* update interrupt enable on  */
#define RTC_UIE_OFF 0x7004 /* update interrupt enable off */

/* ── struct rtc_time — matches Linux uapi ────────────────────────────────── */

struct rtc_time {
	int tm_sec; /* 0-59 */
	int tm_min; /* 0-59 */
	int tm_hour; /* 0-23 */
	int tm_mday; /* 1-31 */
	int tm_mon; /* 0-11  (Linux convention) */
	int tm_year; /* years since 1900 */
	int tm_wday; /* 0-6, Sunday = 0 */
	int tm_yday; /* 0-365 */
	int tm_isdst;
};

/* Share the hardware snapshot and calendar conversion with boot timekeeping. */
static void rtc_read_time(struct rtc_time *t)
{
	struct time_calendar calendar, jan1;
	time_rtc_calendar(&calendar);
	t->tm_sec = calendar.sec;
	t->tm_min = calendar.min;
	t->tm_hour = calendar.hour;
	t->tm_mday = calendar.mday;
	t->tm_mon = calendar.mon - 1;
	t->tm_year = calendar.year - 1900;
	unsigned long epoch = time_rtc_epoch(&calendar);
	jan1 = calendar;
	jan1.mon = jan1.mday = 1;
	jan1.hour = jan1.min = jan1.sec = 0;
	t->tm_wday = (epoch / 86400 + 4) % 7;
	t->tm_yday = (epoch - time_rtc_epoch(&jan1)) / 86400;
	t->tm_isdst = 0;
}

/* ── VFS file operations ─────────────────────────────────────────────────── */

/* Set to 1 while UIE is active; rtc_read delivers one synthetic tick. */
static int rtc_uie_enabled;

static ssize_t rtc_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	/*
	 * When UIE is on, hwclock reads an unsigned long interrupt-count word
	 * to synchronise to the next 1 Hz boundary.  Return a count of 1 so
	 * the read completes immediately rather than blocking indefinitely.
	 */
	if (rtc_uie_enabled && size >= sizeof(unsigned long)) {
		unsigned long cnt = 1;
		memcpy(buf, &cnt, sizeof(cnt));
		return (ssize_t)sizeof(cnt);
	}
	return 0;
}

static unsigned rtc_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return (events & FS_POLL_WRITE) ? FS_POLL_WRITE : 0;
}

static int rtc_ioctl_rtc_rd_time(void *context __attribute__((unused)),
				 unsigned cmd __attribute__((unused)),
				 void *buf __attribute__((unused)))
{
	struct rtc_time t;
	rtc_read_time(&t);
	memcpy(buf, &t, sizeof(t));
	return 0;
}

static int rtc_ioctl_rtc_uie_on(void *context __attribute__((unused)),
				unsigned cmd __attribute__((unused)),
				void *buf __attribute__((unused)))
{
	rtc_uie_enabled = 1;
	return 0;
}

static int rtc_ioctl_rtc_uie_off(void *context __attribute__((unused)),
				 unsigned cmd __attribute__((unused)),
				 void *buf __attribute__((unused)))
{
	rtc_uie_enabled = 0;
	return 0;
}

static const command_operation rtc_commands[256] = {
	[RTC_RD_TIME & 255] = { RTC_RD_TIME, rtc_ioctl_rtc_rd_time },
	[RTC_UIE_ON & 255] = { RTC_UIE_ON, rtc_ioctl_rtc_uie_on },
	[RTC_UIE_OFF & 255] = { RTC_UIE_OFF, rtc_ioctl_rtc_uie_off },
};

static const command_operation *const rtc_command_groups[256] = {
	[(RTC_RD_TIME >> 8) & 255] = rtc_commands,
};

static int rtc_ioctl(file *fp, unsigned cmd, void *buf)
{
	return command_dispatch(rtc_command_groups, fp, cmd, buf, -ENOSYS);
}

static int rtc_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;

	memset(s, 0, sizeof(*s));
	s->st_atime = s->st_mtime = s->st_ctime = time_wall_sec();
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(10, 0);
	s->st_rdev = MKDEV(10, 135);
	s->st_nlink = 1;
	return 0;
}

static int rtc_release(file *fp)
{
	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const file_operations rtc_fops = {
	.getattr = rtc_getattr,
	.read = rtc_read,
	.poll = rtc_poll,
	.ioctl = rtc_ioctl,
	.release = rtc_release,
};

/* ── cdev dispatch ───────────────────────────────────────────────────────── */

static file *rtc_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &rtc_fops;
	return fp;
}

/* ── Registration ────────────────────────────────────────────────────────── */

static void rtc_dev_register(super_block *dev_sb)
{
	printk("dev: registered /dev/rtc\n");
	cdev_register_named(S_IFCHR, RTC_MAJOR, RTC_MINOR, 1, "misc",
			    rtc_cdev_open);
	vfs_mknod(dev_sb, "/rtc", S_IFCHR | 0660, MKDEV(RTC_MAJOR, RTC_MINOR));
}

DEV_INIT(rtc_dev_register);
