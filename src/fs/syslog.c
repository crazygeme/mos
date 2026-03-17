/*
 * src/fs/syslog.c — kernel syslog ring buffer and syslog(2) syscall.
 *
 * The ring buffer captures every string written through klog_writestr()
 * (the serial-log path).  Userspace reads it via sys_syslog() using the
 * standard Linux klogctl type values.
 */

#include <fs/syslog.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <errno.h>
#include <macro.h>

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

#define SYSLOG_BUF_SIZE (16 * 1024)

#define SYSLOG_ACTION_CLOSE 0
#define SYSLOG_ACTION_OPEN 1
#define SYSLOG_ACTION_READ 2
#define SYSLOG_ACTION_READ_ALL 3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR 5
#define SYSLOG_ACTION_CONSOLE_OFF 6
#define SYSLOG_ACTION_CONSOLE_ON 7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD 9
#define SYSLOG_ACTION_SIZE_BUFFER 10

static char syslog_buf[SYSLOG_BUF_SIZE];
static unsigned syslog_head = 0; /* next write position          */
static unsigned syslog_tail = 0; /* next consume position        */
static unsigned syslog_used = 0; /* bytes available to read      */
static spinlock_t syslog_lock;
static int syslog_inited = 0;

static void syslog_init(void)
{
	spinlock_init(&syslog_lock);
	syslog_inited = 1;
}

KERNEL_INIT(1, syslog_init);

/* ── Public write (called from kprint.c) ────────────────────────────────── */

void syslog_write(const char *str, unsigned len)
{
	unsigned i;

	if (!syslog_inited || !str || !len)
		return;

	spinlock_lock(&syslog_lock);
	for (i = 0; i < len; i++) {
		syslog_buf[syslog_head] = str[i];
		syslog_head = (syslog_head + 1) % SYSLOG_BUF_SIZE;
		if (syslog_used < SYSLOG_BUF_SIZE) {
			syslog_used++;
		} else {
			/* Buffer full: overwrite oldest byte */
			syslog_tail = (syslog_tail + 1) % SYSLOG_BUF_SIZE;
		}
	}
	spinlock_unlock(&syslog_lock);
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Copy up to n unread bytes from the ring buffer into buf.
 * Caller must hold syslog_lock.  Does not consume (advance tail). */
static int syslog_copy_locked(char *buf, int n)
{
	int copy = (n < (int)syslog_used) ? n : (int)syslog_used;
	int i;

	for (i = 0; i < copy; i++)
		buf[i] = syslog_buf[(syslog_tail + i) % SYSLOG_BUF_SIZE];

	return copy;
}

/* ── sys_syslog ──────────────────────────────────────────────────────────── */

int sys_syslog(int type, char *buf, int len)
{
	int ret = 0;

	switch (type) {
	case SYSLOG_ACTION_CLOSE:
	case SYSLOG_ACTION_OPEN:
		break;

	case SYSLOG_ACTION_READ:
		if (!buf || len < 0)
			return -EINVAL;
		spinlock_lock(&syslog_lock);
		ret = syslog_copy_locked(buf, len);
		syslog_tail = (syslog_tail + ret) % SYSLOG_BUF_SIZE;
		syslog_used -= ret;
		spinlock_unlock(&syslog_lock);
		break;

	case SYSLOG_ACTION_READ_ALL:
		if (!buf || len < 0)
			return -EINVAL;
		spinlock_lock(&syslog_lock);
		ret = syslog_copy_locked(buf, len);
		spinlock_unlock(&syslog_lock);
		break;

	case SYSLOG_ACTION_READ_CLEAR:
		if (!buf || len < 0)
			return -EINVAL;
		spinlock_lock(&syslog_lock);
		ret = syslog_copy_locked(buf, len);
		syslog_head = syslog_tail = syslog_used = 0;
		spinlock_unlock(&syslog_lock);
		break;

	case SYSLOG_ACTION_CLEAR:
		spinlock_lock(&syslog_lock);
		syslog_head = syslog_tail = syslog_used = 0;
		spinlock_unlock(&syslog_lock);
		break;

	case SYSLOG_ACTION_CONSOLE_OFF:
	case SYSLOG_ACTION_CONSOLE_ON:
	case SYSLOG_ACTION_CONSOLE_LEVEL:
		break;

	case SYSLOG_ACTION_SIZE_UNREAD:
		spinlock_lock(&syslog_lock);
		ret = (int)syslog_used;
		spinlock_unlock(&syslog_lock);
		break;

	case SYSLOG_ACTION_SIZE_BUFFER:
		ret = SYSLOG_BUF_SIZE;
		break;

	default:
		return -EINVAL;
	}

	return ret;
}
