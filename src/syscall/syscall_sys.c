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
	return 0;
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

int sys_time(unsigned *t)
{
	if (!t)
		return -1;

	*t = (unsigned)(time_wall_us() / 1000000ULL);

	if (TestControl.verbos)
		klog("time() = %u\n", *t);

	return 0;
}

int sys_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	long long now;

	if (!tv)
		return -EFAULT;

	now = (long long)time_wall_us();
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

long sys_times(struct tms *buf)
{
	if (TestControl.verbos)
		klog("times\n");

	if (buf) {
		buf->tms_utime = 0;
		buf->tms_stime = 0;
		buf->tms_cutime = 0;
		buf->tms_cstime = 0;
	}
	/* Return clock ticks since boot; HZ=100 → divide µs by 10000. */
	return (long)(time_wall_us() / (1000000ULL / HZ));
}

int sys_setpriority(int which, int who, int prio)
{
	if (TestControl.verbos)
		klog("setpriority(%d, %d, %d)\n", which, who, prio);

	/* No real scheduling priority support; silently accept. */
	return 0;
}

int sys_vhangup(void)
{
	if (TestControl.verbos)
		klog("vhangup\n");

	/* Virtual hangup on the controlling terminal — no-op. */
	return 0;
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
	info->uptime = (long)(time_wall_us() / 1000000ULL);
	info->totalram = (unsigned long)total_pages * PAGE_SIZE;
	info->freeram = (unsigned long)free_pages * PAGE_SIZE;
	info->mem_unit = 1;

	if (TestControl.verbos)
		klog("sysinfo: total=%luKB free=%luKB\n", info->totalram / 1024,
		     info->freeram / 1024);

	return 0;
}

int sys_getpriority(int which, int who)
{
	if (TestControl.verbos)
		klog("getpriority(%d, %d)\n", which, who);

	/* No real priority; return 0 (maps to nice 0). */
	return 0;
}

int sys_ioperm(unsigned long from, unsigned long num, int turn_on)
{
	if (TestControl.verbos)
		klog("ioperm(%lx, %lx, %d)\n", from, num, turn_on);

	return -EPERM;
}

int sys_iopl(int level)
{
	if (TestControl.verbos)
		klog("iopl(%d)\n", level);

	return -EPERM;
}

int sys_quotactl(int cmd, const char *special, int id, void *addr)
{
	if (TestControl.verbos)
		klog("quotactl\n");

	return -ENOSYS;
}

#define MREMAP_MAYMOVE 1

int sys_mremap(unsigned old_addr, unsigned old_size, unsigned new_size,
	       int flags, unsigned new_addr)
{
#if 0
	task_struct *cur = CURRENT_TASK();
	vm_region *region;
	unsigned old_size_pg, new_size_pg, old_end;
	int ret;

	if (TestControl.verbos)
		klog("mremap(%x, %x, %x, flags=%x)\n", old_addr, old_size,
		     new_size, flags);

	if (old_addr & (PAGE_SIZE - 1))
		return -EINVAL;

	old_size_pg = (old_size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
	new_size_pg = (new_size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;

	if (!new_size_pg)
		return -EINVAL;

	region = vm_find_map(cur->user->vm, old_addr);
	if (!region)
		return -EFAULT;

	if (new_size_pg == old_size_pg)
		return (int)old_addr;

	if (new_size_pg < old_size_pg) {
		do_munmap((void *)(old_addr + new_size_pg),
			  old_size_pg - new_size_pg);
		return (int)old_addr;
	}

	/* Grow: try in-place if space after the region is free. */
	old_end = old_addr + old_size_pg;
	if (!vm_find_map(cur->user->vm, old_end)) {
		ret = do_mmap(old_end, new_size_pg - old_size_pg, region->prot,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (ret >= 0)
			return (int)old_addr;
	}

	if (!(flags & MREMAP_MAYMOVE))
		return -ENOMEM;

	/* Move: allocate a new region, copy, free the old one. */
	ret = do_mmap(0, new_size_pg, region->prot, MAP_PRIVATE | MAP_ANONYMOUS,
		      -1, 0);
	if (ret < 0)
		return ret;

	memcpy((void *)ret, (void *)old_addr, old_size_pg);
	do_munmap((void *)old_addr, old_size_pg);
	return ret;
#endif
	return -ENOSYS;
}

/* mlock/munlock/mlockall/munlockall — no-ops: we never swap, all pages resident */
int sys_mlock(const void *addr, size_t len)
{
	(void)addr;
	(void)len;
	return 0;
}

int sys_munlock(const void *addr, size_t len)
{
	(void)addr;
	(void)len;
	return 0;
}

int sys_mlockall(int flags)
{
	(void)flags;
	return 0;
}

int sys_munlockall(void)
{
	return 0;
}

int sys_query_module(const char *name, int which, void *buf, size_t bufsize,
		     size_t *ret)
{
	// No module support, always return success
	if (TestControl.verbos)
		klog("query_module\n");

	if (buf && bufsize > 0) {
		char *p = (char *)buf;
		*p = '\0';
	}

	if (ret)
		*ret = 0;

	return 0;
}
