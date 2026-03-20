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
#include <hw/time.h>
#include <dev/dev.h>
#include <macro.h>
#include <errno.h>
#include <unistd.h>

/* ── Linux-compatible RTC ioctls ─────────────────────────────────────────── */

#define RTC_RD_TIME 0x80247009 /* read time */

/* ── CMOS / MC146818A register map ───────────────────────────────────────── */

#define CMOS_REG_SET 0x70 /* index port */
#define CMOS_REG_IO 0x71 /* data port  */

#define RTC_REG_SEC 0x00
#define RTC_REG_MIN 0x02
#define RTC_REG_HOUR 0x04
#define RTC_REG_MDAY 0x07
#define RTC_REG_MON 0x08
#define RTC_REG_YEAR 0x09
#define RTC_REG_A 0x0a /* Register A */

#define RTCSA_UIP 0x80 /* update-in-progress flag in Register A */

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

/* ── CMOS helpers ────────────────────────────────────────────────────────── */

static unsigned char cmos_read(unsigned char reg)
{
	port_write_byte(CMOS_REG_SET, reg);
	return port_read_byte(CMOS_REG_IO);
}

static int bcd_to_bin(unsigned char x)
{
	return (x & 0x0f) + ((x >> 4) * 10);
}

/*
 * rtc_read_time — read wall-clock fields from CMOS into *t.
 *
 * Waits for the update-in-progress flag to clear, then re-reads until two
 * consecutive seconds values agree (same strategy as Linux and time.c).
 * CMOS delivers BCD values; we convert to binary.
 * Two-digit years < 70 are assumed to be 2000+.
 */
static void rtc_read_time(struct rtc_time *t)
{
	int sec, min, hour, mday, mon, year;

	do {
		while (cmos_read(RTC_REG_A) & RTCSA_UIP)
			; /* wait for update to finish */
		sec = bcd_to_bin(cmos_read(RTC_REG_SEC));
		min = bcd_to_bin(cmos_read(RTC_REG_MIN));
		hour = bcd_to_bin(cmos_read(RTC_REG_HOUR));
		mday = bcd_to_bin(cmos_read(RTC_REG_MDAY));
		mon = bcd_to_bin(cmos_read(RTC_REG_MON));
		year = bcd_to_bin(cmos_read(RTC_REG_YEAR));
	} while (sec != bcd_to_bin(cmos_read(RTC_REG_SEC)));

	if (year < 70)
		year += 100; /* 2000-2069: CMOS gives 0-69, map to 100-169 */

	t->tm_sec = sec;
	t->tm_min = min;
	t->tm_hour = hour;
	t->tm_mday = mday;
	t->tm_mon = mon - 1; /* CMOS is 1-based; Linux wants 0-based */
	t->tm_year = year; /* years since 1900 */
	t->tm_wday = 0;
	t->tm_yday = 0;
	t->tm_isdst = 0;
}

/* ── VFS file operations ─────────────────────────────────────────────────── */

static ssize_t rtc_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	/* No interrupt-based event counter implemented; behave like
	 * a non-blocking read with nothing available. */
	return 0;
}

static int rtc_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_READ)
		return -1; /* no RTC interrupt events */
	if (type == FS_POLL_EXCEPT)
		return -1;
	return 0;
}

static int rtc_ioctl(file *fp, unsigned cmd, void *buf)
{
	struct rtc_time t;

	switch (cmd) {
	case RTC_RD_TIME:
		rtc_read_time(&t);
		memcpy(buf, &t, sizeof(t));
		return 0;
	}
	return -ENOSYS;
}

static int rtc_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_dev = 0xa; /* misc major */
	s->st_rdev = (10 << 8) | 135; /* major 10, minor 135 */
	s->st_nlink = 1;
	return 0;
}

static int rtc_release(file *fp)
{
	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const inode_operations rtc_iops = {
	.getattr = rtc_getattr,
};

static const file_operations rtc_fops = {
	.read = rtc_read,
	.poll = rtc_poll,
	.ioctl = rtc_ioctl,
	.release = rtc_release,
};

/* ── Superblock ──────────────────────────────────────────────────────────── */

static file *rtc_open_root(super_block *sb)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	node->i_op = &rtc_iops;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &rtc_fops;
	return fp;
}

static super_operations rtc_sops = {
	.open_root = rtc_open_root,
};

/* ── Registration ────────────────────────────────────────────────────────── */

static void rtc_dev_register(super_block *dev_sb)
{
	vfs_mount(dev_sb, "/rtc", sget(&rtc_sops));
}

DEV_INIT(rtc_dev_register);
