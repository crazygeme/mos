/*
 * syscall_sys.c — miscellaneous system syscall handlers.
 *
 * Covers: uname, sethostname, utime, time, gettimeofday, nanosleep,
 *         reboot, socketcall, mmap, munmap, mprotect, umask.
 */

#include <ps/ps.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <unistd.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include "syscall_internal.h"

extern unsigned phymm_used;

struct sysinfo {
	long uptime;
	unsigned long loads[3];
	unsigned long totalram;
	unsigned long freeram;
	unsigned long sharedram;
	unsigned long bufferram;
	unsigned long totalswap;
	unsigned long freeswap;
	unsigned short procs;
	unsigned long totalhigh;
	unsigned long freehigh;
	unsigned int mem_unit;
	char _f[8];
};

static char sys_hostname[_SYS_NAMELEN] = "qemu-mos";

/*
 * Wall-clock offset: difference (in us) between the real-world epoch and
 * the kernel's PIT tick counter.  Set by sys_settimeofday (syscall 79) and
 * read back by sys_gettimeofday / sys_time.
 *
 * 0 means "not set by userspace" — fall back to the RTC-derived boot epoch.
 */
static long long g_wall_offset_us;

/* Return current wall-clock time in microseconds since Unix epoch. */
static unsigned long long wall_clock_us(void)
{
	if (g_wall_offset_us != 0)
		return (unsigned long long)((long long)time_now_us() +
					    g_wall_offset_us);
	/* settimeofday never called: use RTC boot epoch + uptime. */
	return (unsigned long long)time_unix_sec() * 1000000ULL;
}

int sys_uname(struct utsname *utname)
{
	if (TestControl.verbos)
		klog("uname\n");

	strcpy(utname->sysname, UTS_SYSNAME);
	strcpy(utname->nodename, sys_hostname);
	strcpy(utname->release, UTS_RELEASE);
	strcpy(utname->version, UTS_VERSION);
	strcpy(utname->machine, UTS_MACHINE);
	strcpy(utname->domain, UTS_NODENAME);
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

	*t = (unsigned)(wall_clock_us() / 1000000ULL);

	if (TestControl.verbos)
		klog("time() = %u\n", *t);

	return 0;
}

int sys_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	long long now;

	if (!tv)
		return -EFAULT;

	now = (long long)wall_clock_us();
	us_to_timeval((unsigned long long)now, tv);

	if (tz)
		tz->tz_minuteswest = tz->tz_dsttime = 0;

	if (TestControl.verbos)
		klog("gettimeofday() = %d(sec), %d(usec), while now is %lld(us)\n",
		     tv->tv_sec, tv->tv_usec, now);

	return 0;
}

int sys_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
	if (tv) {
		long long wall_us =
			(long long)tv->tv_sec * 1000000 + tv->tv_usec;
		g_wall_offset_us = wall_us - (long long)time_now_us();

		if (TestControl.verbos)
			klog("settimeofday(%d.%06d) offset=%lld us\n",
			     tv->tv_sec, tv->tv_usec, g_wall_offset_us);
	}
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

	if (TestControl.verbos)
		klog("nanosleep(%d.%09d) = %ums\n", req->tv_sec, req->tv_nsec,
		     total_millisecond);

	msleep(total_millisecond);
	return 0;
}

/* Linux reboot(2) magic numbers */
#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 0x28121969

static unsigned notify_initctl(unsigned cmd)
{
	struct init_request req;
	file *fp;

	memset(&req, 0, sizeof(req));
	req.magic = INIT_MAGIC;
	req.cmd = INIT_CMD_RUNLVL;

	switch (cmd) {
	case MOS_REBOOT_CMD_RESTART:
		req.runlevel = '6';
		break;
	case MOS_REBOOT_CMD_POWER_OFF:
	case MOS_REBOOT_CMD_HALT:
		req.runlevel = '0';
		break;
	default:
		return -EINVAL;
	}

	fp = fs_open_file("/dev/initctl", O_WRONLY | O_NOFOLLOW, NULL);
	if (!fp)
		return -EIO;

	fp->f_fop->write(fp, &req, sizeof(req), &fp->f_pos);
	fs_put_file(fp);

	return 0;
}

int sys_reboot(unsigned magic1, unsigned magic2, unsigned cmd, void *arg)
{
	task_struct *cur = CURRENT_TASK();

	if (TestControl.verbos)
		klog("reboot(magic1=%x, magic2=%x, cmd=%x) from pid %d\n",
		     magic1, magic2, cmd, cur->psid);

	/* Reject calls that don't carry the Linux magic numbers. */
	if (magic1 != LINUX_REBOOT_MAGIC1)
		return -EINVAL;

	/*
	 * CAD_ON / CAD_OFF: enable or disable Ctrl-Alt-Delete.
	 * We have no hardware CAD support, so both are no-ops.
	 */
	if (cmd == MOS_REBOOT_CMD_CAD_ON || cmd == MOS_REBOOT_CMD_CAD_OFF)
		return 0;

	/*
	 * PID 1 (init) calls us after completing an orderly shutdown —
	 * perform the hardware action directly.
	 */
	if (cur->psid == 1) {
		switch (cmd) {
		case MOS_REBOOT_CMD_RESTART:
			reboot();
			break;
		case MOS_REBOOT_CMD_POWER_OFF:
		case MOS_REBOOT_CMD_HALT:
			shutdown();
			break;
		}
		return 0;
	}

	/*
	 * All other callers signal init via /dev/initctl so it can perform
	 * an orderly shutdown before invoking the hardware action.
	 */
	return notify_initctl(cmd);
}

int sys_mmap(struct mmap_arg_struct32 *arg)
{
	if (arg->len > (10 * 1024 * 1024))
		return -1;

	return do_mmap(arg->addr, arg->len, arg->prot, arg->flags, arg->fd,
		       arg->offset);
}

int sys_mmap2(unsigned addr, unsigned len, unsigned prot, unsigned flags,
	      int fd, unsigned pgoffset)
{
	return do_mmap(addr, len, prot, flags, fd, pgoffset * PAGE_SIZE);
}

int sys_munmap(void *addr, unsigned length)
{
	if (TestControl.verbos)
		klog("munmap (%x, %x)\n", addr, length);

	return do_munmap(addr, length);
}

int sys_mprotect(void *addr, unsigned len, int prot)
{
	task_struct *cur = CURRENT_TASK();
	unsigned begin = (unsigned)addr;
	unsigned end, vir;

	if (TestControl.verbos)
		klog("mprotect: addr %x, len %x, prot %x\n", addr, len, prot);

	/* POSIX: addr must be page-aligned */
	if (begin & ~PAGE_SIZE_MASK)
		return -EINVAL;

	if (len == 0)
		return 0;

	end = (begin + len + PAGE_SIZE - 1) & PAGE_SIZE_MASK;

	/* Update VM region descriptors, splitting regions at boundaries. */
	vm_mprotect(cur->user->vm, begin, end, prot);

	/* Update hardware page-table entries for already-faulted-in pages. */
	for (vir = begin; vir < end; vir += PAGE_SIZE) {
		unsigned mmflag = mm_get_map_flag(vir);
		if (mmflag == 0)
			continue; /* not yet mapped; vm descriptor update is enough */

		if (prot & PROT_WRITE)
			mmflag |= PAGE_ENTRY_WRITABLE;
		else
			mmflag &= ~PAGE_ENTRY_WRITABLE;

		mm_set_map_flag(vir, mmflag);
	}

	RELOAD_CR3();
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

int sys_sysinfo(void *buf)
{
	struct sysinfo *info = (struct sysinfo *)buf;
	unsigned total_pages = phymm_end - phymm_begin;
	unsigned free_pages =
		total_pages > phymm_used ? total_pages - phymm_used : 0;

	if (!info)
		return -EFAULT;

	memset(info, 0, sizeof(*info));
	info->uptime = (long)(time_now_us() / 1000000ULL);
	info->totalram = (unsigned long)total_pages * PAGE_SIZE;
	info->freeram = (unsigned long)free_pages * PAGE_SIZE;
	info->mem_unit = 1;

	if (TestControl.verbos)
		klog("sysinfo: total=%luKB free=%luKB\n", info->totalram / 1024,
		     info->freeram / 1024);

	return 0;
}
