/*
 * src/fs/syslog.c — kernel syslog ring buffer and syslog(2) syscall.
 *
 * Backed by a cy_buf so SYSLOG_ACTION_READ blocks when no data is available,
 * matching Linux klogctl(2) semantics that klogd relies on.
 */

#include <fs/syslog.h>
#include <lib/cyclebuf.h>
#include <lib/klib.h>
#include <macro.h>
#include <config.h>
#include <errno.h>

/* ── Buffer ──────────────────────────────────────────────────────────────── */

#define SYSLOG_BUF_PAGES 4 /* 16 KiB */

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

static cy_buf *syslog_cyb;
static int syslog_inited = 0;

static void syslog_init(void)
{
	syslog_cyb = cyb_create(SYSLOG_BUF_PAGES);
	syslog_inited = 1;
}

KERNEL_INIT(1, syslog_init);

/* ── sys_syslog ──────────────────────────────────────────────────────────── */

int sys_syslog(int type, char *buf, int len)
{
	int ret = 0;

	if (TestControl.verbos)
		klog("syslog: type=%d, buf=%x, len=%d\n", type, buf, len);

	switch (type) {
	case SYSLOG_ACTION_CLOSE:
	case SYSLOG_ACTION_OPEN:
		break;

	case SYSLOG_ACTION_READ:
		/* Block until data is available, interruptible by signals. */
		if (!buf || len < 0)
			return -EINVAL;
		ret = cyb_getbuf(syslog_cyb, buf, len, 1, 1);
		if (ret < 0)
			return -EINTR;
		break;

	case SYSLOG_ACTION_READ_ALL:
		/* Non-blocking read of whatever is currently available. */
		if (!buf || len < 0)
			return -EINVAL;
		ret = cyb_getbuf(syslog_cyb, buf, len, 0, 0);
		break;

	case SYSLOG_ACTION_READ_CLEAR:
		/* Read up to len bytes, then discard the rest. */
		if (!buf || len < 0)
			return -EINVAL;
		ret = cyb_getbuf(syslog_cyb, buf, len, 0, 0);
		cyb_flush(syslog_cyb);
		break;

	case SYSLOG_ACTION_CLEAR:
		cyb_flush(syslog_cyb);
		break;

	case SYSLOG_ACTION_CONSOLE_OFF:
	case SYSLOG_ACTION_CONSOLE_ON:
	case SYSLOG_ACTION_CONSOLE_LEVEL:
		break;

	case SYSLOG_ACTION_SIZE_UNREAD:
		ret = cyb_get_buf_len(syslog_cyb);
		break;

	case SYSLOG_ACTION_SIZE_BUFFER:
		ret = SYSLOG_BUF_PAGES * PAGE_SIZE;
		break;

	default:
		return -EINVAL;
	}

	return ret;
}
