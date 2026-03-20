/*
 * syscall_sys.c — miscellaneous system syscall handlers.
 *
 * Covers: uname, sethostname, utime, time, gettimeofday, nanosleep,
 *         reboot, socketcall, mmap, munmap, mprotect, umask.
 */

#include <ps/ps.h>
#include <mm/mmap.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <unistd.h>
#include "syscall_internal.h"

static char sys_hostname[_SYS_NAMELEN] = "qemu-mos";

int sys_uname(struct utsname *utname)
{
	if (TestControl.verbos)
		klog("uname\n");

	strcpy(utname->machine, "i386");
	strcpy(utname->nodename, sys_hostname);
	strcpy(utname->release, "0.91-generic");
	strcpy(utname->sysname, "Mos");
	strcpy(utname->version, "Mos Wed Mar 11 15:00:00 UTC 2026");
	strcpy(utname->domain, "Ender");
	return 1;
}

int sys_sethostname(const char *name, unsigned len)
{
	if (!name || len > (_SYS_NAMELEN - 1))
		return -EINVAL;

	memcpy(sys_hostname, name, len);
	sys_hostname[len] = '\0';

	if (TestControl.verbos)
		klog("sethostname(%s, %d)\n", sys_hostname, len);

	return 0;
}

int sys_utime(const char *filename, const struct utimbuf *times)
{
	if (TestControl.verbos)
		klog("utime\n");

	return 0;
}

int sys_time(unsigned *t)
{
	if (!t)
		return -1;

	*t = (unsigned)time_now_ms();
	return 0;
}

int sys_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	unsigned long long now;

	if (!tv)
		return -EFAULT;

	now = time_now_ms();
	ms_to_timeval(now, tv);

	if (tz)
		tz->tz_minuteswest = tz->tz_dsttime = 0;

	if (TestControl.verbos)
		klog("gettimeofday() = %d(sec), %d(usec), while now is %d(ms)\n",
		     tv->tv_sec, tv->tv_usec, now);

	return 0;
}

int sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
	unsigned int total_millisecond;

	if (!req)
		return -EFAULT;
	if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec > 999999999)
		return -EINVAL;

	total_millisecond = req->tv_sec * 1000 + req->tv_nsec / 1000000;
	msleep(total_millisecond);
	return 0;
}

int sys_reboot(unsigned cmd)
{
	switch (cmd) {
	case MOS_REBOOT_CMD_RESTART:
		reboot();
		break;
	case MOS_REBOOT_CMD_POWER_OFF:
		shutdown();
		break;
	default:
		break;
	}
	return 0;
}

int sys_socketcall(int call, unsigned long *args)
{
	if (TestControl.verbos)
		klog("socketcall(%d, %x)\n", call, args);

	return -1; /* FIXME: no socket support */
}

int sys_mmap(struct mmap_arg_struct32 *arg)
{
	int vir;

	if (arg->len > (10 * 1024 * 1024))
		return -1;

	vir = do_mmap(arg->addr, arg->len, arg->prot, arg->flags, arg->fd,
		      arg->offset);

	return vir;
}

int sys_munmap(void *addr, unsigned length)
{
	if (TestControl.verbos)
		klog("munmap (%x, %x)\n", addr, length);

	return do_munmap(addr, length);
}

int sys_mprotect(void *addr, unsigned len, int prot)
{
	if (TestControl.verbos)
		klog("mprotect: addr %x, len %x, prot %x\n", addr, len, prot);

	return 0;
}

int sys_umask(unsigned mask)
{
	task_struct *cur = CURRENT_TASK();
	int ret = __sync_lock_test_and_set(&cur->umask, (mask & S_IRWXOGU));

	if (TestControl.verbos)
		klog("umask(%d) = %d\n", mask, ret);

	return ret;
}
