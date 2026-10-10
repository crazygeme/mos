/*
 * syscall_sys.c — miscellaneous system syscall handlers.
 *
 * Covers: uname, sethostname, utime, time, gettimeofday, nanosleep,
 *         reboot, socketcall, mmap, munmap, mprotect, umask.
 */

#include <ps/ps.h>
#include <ps/usage.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <mm/mmu.h>
#include <unistd.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <syscall/syscall.h>

extern unsigned phymm_used;

static char sys_hostname[_SYS_NAMELEN] = "qemu-mos";

struct sched_param_k {
	int sched_priority;
};

#define MOS_SCHED_OTHER 0
#define MOS_SCHED_FIFO 1
#define MOS_SCHED_RR 2

static int sched_resolve_task(int pid, task_struct **out)
{
	task_struct *task;

	if (!out)
		return -EINVAL;

	if (pid == 0) {
		*out = CURRENT_TASK();
		return 0;
	}

	task = ps_find_process((unsigned)pid);
	if (!task)
		return -ESRCH;

	*out = task;
	return 0;
}

static int sched_policy_supported(int policy)
{
	return policy == MOS_SCHED_OTHER;
}

int sys_uname(struct utsname *utname)
{
	if (TEST_LOG(TEST_LOG_INFO))
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

	if (TEST_LOG(TEST_LOG_INFO))
		klog("sethostname(%s, %d)\n", sys_hostname, len);

	return 0;
}

int sys_time(unsigned *t)
{
	unsigned now = (unsigned)(time_wall_us() / 1000000ULL);

	if (t)
		*t = now;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("time() = %u\n", now);

	return (int)now;
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

	if (TEST_LOG(TEST_LOG_INFO))
		klog("gettimeofday() = %d(sec), %d(usec), while now is %lld(us)\n",
		     tv->tv_sec, tv->tv_usec, now);

	return 0;
}

static int clock_time_ns(int clockid, unsigned long long *ns)
{
	unsigned long long us;

	switch (clockid) {
	case 0: /* CLOCK_REALTIME */
	case 5: /* CLOCK_REALTIME_COARSE */
		us = time_wall_us();
		break;
	case 1: /* CLOCK_MONOTONIC */
	case 4: /* CLOCK_MONOTONIC_RAW */
	case 6: /* CLOCK_MONOTONIC_COARSE */
	case 7: /* CLOCK_BOOTTIME */
		us = time_now_us();
		break;
	case 2: /* CLOCK_PROCESS_CPUTIME_ID */
		us = (ps_usage_read(&current->thread->user_tickets) +
		      ps_usage_read(&current->thread->kernel_tickets)) *
		     (1000000ULL / HZ);
		break;
	case 3: /* CLOCK_THREAD_CPUTIME_ID */
		us = (task_utime(current) +
		      ps_usage_read(&current->stats->kernel_tickets)) *
		     (1000000ULL / HZ);
		break;
	default:
		return -EINVAL;
	}
	*ns = us * 1000ULL;
	return 0;
}

int sys_clock_gettime(int clockid, struct timespec *tp)
{
	unsigned long long ns;
	int ret;

	if (!tp)
		return -EFAULT;
	ret = clock_time_ns(clockid, &ns);
	if (ret)
		return ret;
	if (ns / 1000000000ULL > 0x7fffffffULL)
		return -EOVERFLOW;
	tp->tv_sec = ns / 1000000000ULL;
	tp->tv_nsec = ns % 1000000000ULL;
	return 0;
}

int sys_clock_gettime64(int clockid, void *tp)
{
	struct {
		int64_t tv_sec;
		int64_t tv_nsec;
	} *result = tp;
	unsigned long long ns;
	int ret;

	if (!result)
		return -EFAULT;
	ret = clock_time_ns(clockid, &ns);
	if (ret)
		return ret;
	result->tv_sec = ns / 1000000000ULL;
	result->tv_nsec = ns % 1000000000ULL;
	return 0;
}

/* CPU clocks retain the timer sample resolution. */
static int clock_resolution(int clockid)
{
	switch (clockid) {
	case 2:
	case 3:
		return 1000000000 / HZ;
	case 0:
	case 1:
	case 4:
	case 5:
	case 6:
	case 7:
		return 1000;
	default:
		return -EINVAL;
	}
}

int sys_clock_getres(int clockid, struct timespec *tp)
{
	int ns = clock_resolution(clockid);
	struct timespec result = { .tv_sec = 0, .tv_nsec = ns };
	if (ns < 0)
		return ns;
	return tp ? ps_write_process_memory(CURRENT_TASK(), tp, &result,
					    sizeof(result)) :
		    0;
}

int sys_clock_getres_time64(int clockid, void *tp)
{
	int ns = clock_resolution(clockid);
	struct {
		int64_t tv_sec, tv_nsec;
	} result = { 0, ns };
	if (ns < 0)
		return ns;
	return tp ? ps_write_process_memory(CURRENT_TASK(), tp, &result,
					    sizeof(result)) :
		    0;
}

int sys_getrandom(void *buf, unsigned len, unsigned flags)
{
	unsigned char bytes[64];
	unsigned copied = 0;
	if (flags & ~3U)
		return -EINVAL;
	if (!buf && len)
		return -EFAULT;
	while (copied < len) {
		unsigned count = len - copied < sizeof(bytes) ? len - copied :
								sizeof(bytes);
		kernel_random_bytes(bytes, count);
		if (ps_write_process_memory(current, (char *)buf + copied,
					    bytes, count) < 0)
			return copied ? (int)copied : -EFAULT;
		copied += count;
	}
	return copied;
}

int sys_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
	if (tv) {
		if (tv->tv_usec < 0 || tv->tv_usec >= 1000000)
			return -EINVAL;
		long long wall_us = (long long)tv->tv_sec * 1000000LL +
				    (long long)tv->tv_usec;
		time_set_wall_offset(wall_us);
	}
	return 0;
}

int sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long long duration_us, start_us, end_us;

	if (!req)
		return -EFAULT;
	if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec > 999999999)
		return -EINVAL;
	duration_us = (unsigned long long)req->tv_sec * 1000000ULL +
		      (req->tv_nsec + 999ULL) / 1000;
	start_us = time_now_us();
	for (;;) {
		unsigned long long elapsed = time_now_us() - start_us;
		if (elapsed >= duration_us || ps_interrupting_signals(cur))
			break;
		unsigned long long left_ms =
			(duration_us - elapsed + 999) / 1000;
		unsigned wait_ms = left_ms > 0xffffffffULL ? 0xffffffffU :
							     (unsigned)left_ms;
		if (!ps_prepare_interruptible_wait(cur, NULL, wait_ms)) {
			task_sched();
			ps_finish_timed_wait(cur);
		}
	}
	end_us = time_now_us();
	if (rem) {
		unsigned long long elapsed = end_us - start_us;
		unsigned long long left =
			duration_us > elapsed ? duration_us - elapsed : 0;
		rem->tv_sec = left / 1000000ULL;
		rem->tv_nsec = (left % 1000000ULL) * 1000;
	}
	return duration_us && ps_interrupting_signals(cur) ? -EINTR : 0;
}

int sys_clock_nanosleep(int clockid, int flags, const struct timespec *req,
			struct timespec *rem)
{
	if (clockid != 0 && clockid != 1)
		return -EINVAL;
	if (flags != 0)
		return -EINVAL;
	return sys_nanosleep(req, rem);
}

int sys_prctl(int option, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
	      uintptr_t arg5)
{
	(void)arg3;
	(void)arg4;
	(void)arg5;
	if (option == 1) { /* PR_SET_PDEATHSIG */
		if (arg2 >= NSIG)
			return -EINVAL;
		current->life->pdeath_signal = arg2;
		return 0;
	}
	if (option == 2) { /* PR_GET_PDEATHSIG */
		int value = current->life->pdeath_signal;
		return ps_write_process_memory(current, (void *)arg2, &value,
					       sizeof(value));
	}
	/* agetty queries dumpability before opening its console. */
	if (option == 3) /* PR_GET_DUMPABLE */
		return 1;
	if (option == 4) /* PR_SET_DUMPABLE */
		return arg2 <= 2 ? 0 : -EINVAL;
	if (option == 7) /* PR_GET_KEEPCAPS */
		return current->credentials->keep_capabilities;
	if (option == 8) { /* PR_SET_KEEPCAPS */
		if (arg2 > 1)
			return -EINVAL;
		current->credentials->keep_capabilities = arg2;
		return 0;
	}
	return -EINVAL;
}

/* Linux reboot(2) magic numbers */
#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 0x28121969

int sys_sched_setparam(int pid, const void *param)
{
	task_struct *task;
	const struct sched_param_k *sp = param;
	int ret;

	if (!sp)
		return -EINVAL;

	ret = sched_resolve_task(pid, &task);
	if (ret < 0)
		return ret;

	if (sp->sched_priority != 0)
		return -EINVAL;

	/* MOS currently implements only SCHED_OTHER semantics. */
	(void)task;
	return 0;
}

int sys_sched_getparam(int pid, void *param)
{
	task_struct *task;
	struct sched_param_k *sp = param;
	int ret;

	if (!sp)
		return -EINVAL;

	ret = sched_resolve_task(pid, &task);
	if (ret < 0)
		return ret;

	sp->sched_priority = 0;
	(void)task;
	return 0;
}

int sys_sched_setscheduler(int pid, int policy, const void *param)
{
	const struct sched_param_k *sp = param;
	int ret;

	if (!sched_policy_supported(policy))
		return -EINVAL;
	if (!sp)
		return -EINVAL;

	ret = sys_sched_setparam(pid, param);
	if (ret < 0)
		return ret;

	return policy;
}

int sys_sched_getscheduler(int pid)
{
	task_struct *task;
	int ret = sched_resolve_task(pid, &task);

	if (ret < 0)
		return ret;

	(void)task;
	return MOS_SCHED_OTHER;
}

int sys_sched_get_priority_max(int algorithm)
{
	if (!sched_policy_supported(algorithm))
		return -EINVAL;
	return 0;
}

int sys_sched_get_priority_min(int algorithm)
{
	if (!sched_policy_supported(algorithm))
		return -EINVAL;
	return 0;
}

int sys_sched_rr_get_interval(int pid, struct timespec *tp)
{
	task_struct *task;
	int ret;

	if (!tp)
		return -EINVAL;

	ret = sched_resolve_task(pid, &task);
	if (ret < 0)
		return ret;

	tp->tv_sec = 0;
	tp->tv_nsec = 0;
	(void)task;
	return 0;
}

int sys_reboot(unsigned magic1, unsigned magic2, unsigned cmd, void *arg)
{
	task_struct *cur = CURRENT_TASK();
	(void)arg;

	if (!cur->execution || cur->credentials->euid != 0)
		return -EPERM;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("reboot(magic1=%x, magic2=%x, cmd=%x) from pid %d\n",
		     magic1, magic2, cmd, cur->life->psid);

	/* Reject calls that don't carry the Linux magic numbers. */
	if (magic1 != LINUX_REBOOT_MAGIC1 ||
	    (magic2 != LINUX_REBOOT_MAGIC2 && magic2 != 0x05121996 &&
	     magic2 != 0x16041998 && magic2 != 0x20112000))
		return -EINVAL;

	/*
	 * CAD_ON / CAD_OFF: enable or disable Ctrl-Alt-Delete.
	 * We have no hardware CAD support, so both are no-ops.
	 */
	if (cmd == MOS_REBOOT_CMD_CAD_ON || cmd == MOS_REBOOT_CMD_CAD_OFF)
		return 0;

	/*
	 * Userspace requests runlevel transitions through init. The terminal
	 * reboot syscall performs the hardware action for any privileged caller,
	 * including the halt and reboot processes running in the shutdown scripts.
	 */
	switch (cmd) {
	case MOS_REBOOT_CMD_POWER_OFF:
	case MOS_REBOOT_CMD_HALT:
		shutdown();
		return 0;
	case MOS_REBOOT_CMD_RESTART:
		reboot();
		return 0;
	default:
		return -EINVAL;
	}
}

int sys_mmap(struct mmap_arg_struct32 *arg)
{
	return do_mmap(arg->addr, arg->len, arg->prot, arg->flags, arg->fd,
		       arg->offset);
}

int sys_mmap2(unsigned addr, unsigned len, unsigned prot, unsigned flags,
	      int fd, unsigned pgoffset)
{
	return do_mmap(addr, len, prot, flags, fd,
		       (uint64_t)pgoffset * PAGE_SIZE);
}

int sys_munmap(void *addr, size_t length)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("munmap (%x, %x)\n", addr, length);

	return do_munmap(addr, length);
}

int sys_mprotect(void *addr, size_t len, int prot)
{
	task_struct *cur = CURRENT_TASK();
	LOCK_GUARD(&cur->memory->mapping_lock);
	vaddr_t begin = (vaddr_t)(uintptr_t)addr;
	vaddr_t end, vir;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("mprotect: addr %x, len %x, prot %x\n", addr, len, prot);

	if (begin >= cur->memory->task_size ||
	    len > cur->memory->task_size - begin)
		return -EINVAL;
	if (!arch_mm_user_range_valid(begin, len))
		return -EINVAL;
	/* POSIX: addr must be page-aligned */
	if (begin & ~PAGE_SIZE_MASK)
		return -EINVAL;

	if (len == 0)
		return 0;

	end = (begin + len + PAGE_SIZE - 1) & PAGE_SIZE_MASK;

	/* Update VM region descriptors, splitting regions at boundaries. */
	vm_mprotect(cur->memory, begin, end, prot);

	/* Update hardware page-table entries for already-faulted-in pages. */
	for (vir = mm_next_mapped_page(begin, end); vir < end;
	     vir = mm_next_mapped_page(vir + PAGE_SIZE, end)) {
		unsigned mmflag = mm_get_map_flag(vir);
		if (mmflag == 0)
			continue; /* not yet mapped; vm descriptor update is enough */

		if (prot == PROT_NONE) {
			/* Force a user-mode fault on any access to guard pages. */
			mmflag &= ~(PAGE_ENTRY_DPL_USER | PAGE_ENTRY_WRITABLE);
			mm_set_map_flag(vir, mmflag);
			continue;
		}

#if MOS_PAGE_NO_EXEC
		mmflag = prot & PROT_EXEC ? mmflag & ~PAGE_ENTRY_NO_EXEC :
					    mmflag | PAGE_ENTRY_NO_EXEC;
#endif
		mmflag |= PAGE_ENTRY_DPL_USER;
		if (!(prot & PROT_WRITE))
			mmflag &= ~PAGE_ENTRY_WRITABLE;
		else {
			vm_region *region = vm_find_map(cur->memory, vir);

			/* Managed pages retain write faults for COW and dirty tracking. */
			if (region &&
			    (region->vm_flags & VM_REGION_F_DIRECT_PHYS))
				mmflag |= PAGE_ENTRY_WRITABLE;
		}

		mm_set_map_flag(vir, mmflag);
	}

	vm_invalidate_task_cache(cur);
	return 0;
}

int sys_madvise(void *addr, size_t length, int advice)
{
	task_struct *cur = CURRENT_TASK();
	mm_struct *mm = cur->memory;
	vaddr_t begin = (vaddr_t)addr, end, cursor;
	int result = 0;
	LOCK_GUARD(&mm->mapping_lock);

	if (begin & (PAGE_SIZE - 1))
		return -EINVAL;
	if (begin >= mm->task_size || length > mm->task_size - begin ||
	    !arch_mm_user_range_valid(begin, length))
		return -EINVAL;
	/* Advisory access patterns and dump selection require no page changes. */
	switch (advice) {
	case 0:
	case 1:
	case 2:
	case 3:
	case 4:
	case 12:
	case 16:
	case 17:
		break;
	default:
		return -EINVAL;
	}
	end = begin + ((length + PAGE_SIZE - 1) & PAGE_SIZE_MASK);
	for (cursor = begin; cursor < end;) {
		vm_region *region = vm_find_vma(mm, cursor);
		vaddr_t limit;

		if (!region || region->begin >= end) {
			result = -ENOMEM;
			break;
		}
		if (region->begin > cursor) {
			result = -ENOMEM;
			cursor = region->begin;
		}
		limit = region->end < end ? region->end : end;
		if (advice == 4) { /* MADV_DONTNEED retains the VMA. */
			if (region->vm_flags & VM_REGION_F_DIRECT_PHYS)
				return -EINVAL;
			if ((region->flag & MAP_SHARED) && region->fp)
				vm_flush_file_dirty(mm, region->fp);
			vm_region_lock_fault(region);
			for (cursor = mm_next_mapped_page(cursor, limit); cursor < limit;
			     cursor = mm_next_mapped_page(cursor + PAGE_SIZE, limit))
				mm_unmap_page(cursor);
			vm_region_unlock_fault(region);
		} else {
			cursor = limit;
		}
	}
	return result;
}

int sys_umask(unsigned mask)
{
	task_struct *cur = CURRENT_TASK();
	int ret = __sync_lock_test_and_set(&cur->fs->umask, (mask & S_IRWXOGU));

	if (TEST_LOG(TEST_LOG_INFO))
		klog("umask(%d) = %d\n", mask, ret);

	return ret;
}

long sys_times(struct tms *buf)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("times\n");

	if (buf) {
		task_thread *usage = current->thread;
		buf->tms_utime = ps_usage_read(&usage->user_tickets);
		buf->tms_stime = ps_usage_read(&usage->kernel_tickets);
		buf->tms_cutime = ps_usage_read(&usage->child_utime);
		buf->tms_cstime = ps_usage_read(&usage->child_stime);
	}
	/* Return clock ticks since boot; HZ=100 → divide µs by 10000. */
	return (long)(time_now_us() / (1000000ULL / HZ));
}

int sys_setpriority(int which, int who, int prio)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("setpriority(%d, %d, %d)\n", which, who, prio);

	/* No real scheduling priority support; silently accept. */
	return 0;
}

int sys_vhangup(void)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("vhangup\n");

	/* Virtual hangup on the controlling terminal — no-op. */
	return 0;
}

int sys_sysinfo(void *buf)
{
	struct mos_sysinfo *info = buf;
	phymm_usage usage;
	unsigned total_pages, free_pages;

	if (!info)
		return -EFAULT;
	phymm_get_usage(&usage);
	total_pages = usage.low_total_pages + usage.high_total_pages;
	free_pages = usage.low_free_pages + usage.high_free_pages;

	memset(info, 0, sizeof(*info));
	info->uptime = (long)(time_now_us() / 1000000ULL);
	/* i386 counts remain representable above 4 GiB by using page units. */
	info->totalram = total_pages;
	info->freeram = free_pages;
	info->totalhigh = usage.high_total_pages;
	info->freehigh = usage.high_free_pages;
	info->mem_unit = PAGE_SIZE;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("sysinfo: total=%luKB free=%luKB\n",
		     (unsigned long)total_pages * (PAGE_SIZE / 1024),
		     (unsigned long)free_pages * (PAGE_SIZE / 1024));

	return 0;
}

int sys_getpriority(int which, int who)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("getpriority(%d, %d)\n", which, who);

	/* No real priority; return 0 (maps to nice 0). */
	return 0;
}

int sys_ioperm(unsigned long from, unsigned long num, int turn_on)
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_INFO))
		klog("ioperm(%lx, %lx, %d)\n", from, num, turn_on);

	if (!cur->execution || cur->credentials->euid != 0)
		return -EPERM;
	return ps_set_ioperm(cur, from, num, turn_on);
}

int sys_iopl(int level)
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_INFO))
		klog("iopl(%d)\n", level);

	if (!cur->execution || cur->credentials->euid != 0)
		return -EPERM;
	if (level < 0 || level > 3)
		return -EINVAL;

	cur->execution->io_priv_level = (unsigned char)level;
	cur->execution->io_allow_all = level != 0;

	reset_tss(cur);
	return 0;
}

int sys_quotactl(int cmd, const char *special, int id, void *addr)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("quotactl\n");

	return -ENOSYS;
}

#define MREMAP_MAYMOVE 1

intptr_t sys_mremap(vaddr_t old_addr, size_t old_size, size_t new_size,
		    int flags, vaddr_t new_addr)
{
	task_struct *cur = CURRENT_TASK();
	LOCK_GUARD(&cur->memory->mapping_lock);
	vm_region *region;
	size_t old_size_pg, new_size_pg;
	vaddr_t old_end, new_end;
	intptr_t ret;

	(void)new_addr;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("mremap(%lx, %lu, %lu, flags=%x)\n",
		     (unsigned long)old_addr, (unsigned long)old_size,
		     (unsigned long)new_size, flags);

	if (old_addr & (PAGE_SIZE - 1))
		return -EINVAL;

	old_size_pg = (old_size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
	new_size_pg = (new_size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;

	if (!new_size_pg)
		return -EINVAL;

	region = vm_find_map(cur->memory, old_addr);
	if (!region)
		return -EFAULT;

	if (new_size_pg == old_size_pg)
		return (intptr_t)old_addr;

	if (new_size_pg < old_size_pg) {
		do_munmap((void *)(uintptr_t)(old_addr + new_size_pg),
			  old_size_pg - new_size_pg);
		return (intptr_t)old_addr;
	}

	/* Grow: extend the existing VMA if the full new range is free. */
	old_end = old_addr + old_size_pg;
	new_end = old_addr + new_size_pg;
	if (vm_extend_map(cur->memory, old_addr, old_end, new_end)) {
		vm_invalidate_task_cache(cur);
		return (intptr_t)old_addr;
	}

	if (!(flags & MREMAP_MAYMOVE))
		return -ENOMEM;

	/* Move: allocate a new region, copy, free the old one. */
	ret = do_mmap(0, new_size_pg, region->prot, MAP_PRIVATE | MAP_ANONYMOUS,
		      -1, 0);
	if (ret < 0)
		return ret;

	memcpy((void *)(uintptr_t)ret, (void *)(uintptr_t)old_addr,
	       old_size_pg);
	do_munmap((void *)(uintptr_t)old_addr, old_size_pg);
	return ret;
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
	if (TEST_LOG(TEST_LOG_INFO))
		klog("query_module\n");

	if (buf && bufsize > 0) {
		char *p = (char *)buf;
		*p = '\0';
	}

	if (ret)
		*ret = 0;

	return 0;
}

/* 17: break — predates virtual memory, permanently obsolete */
int sys_break(void)
{
	return -ENOSYS;
}

/* 25: stime — set system time from a time_t pointer */
int sys_stime(unsigned *t)
{
	task_struct *cur = CURRENT_TASK();

	if (!t)
		return -EFAULT;
	if (cur->credentials->euid != 0)
		return -EPERM;

	time_set_wall_offset((long long)(*t) * 1000000LL);

	if (TEST_LOG(TEST_LOG_INFO))
		klog("stime(%u)\n", *t);

	return 0;
}

/* 31: stty — obsolete tty ioctl interface */
int sys_stty(void)
{
	return -ENOSYS;
}

/* 32: gtty — obsolete tty ioctl interface */
int sys_gtty(void)
{
	return -ENOSYS;
}

struct timeb {
	unsigned time;
	unsigned short millitm;
	short timezone;
	short dstflag;
};

/* 35: ftime — return time in old BSD timeb struct */
int sys_ftime(void *buf)
{
	struct timeb *tp = (struct timeb *)buf;
	unsigned long long now_us;

	if (!tp)
		return -EFAULT;

	now_us = time_wall_us();
	tp->time = (unsigned)(now_us / 1000000ULL);
	tp->millitm = (unsigned short)((now_us % 1000000ULL) / 1000ULL);
	tp->timezone = 0;
	tp->dstflag = 0;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("ftime() = %u.%03u\n", tp->time, tp->millitm);

	return 0;
}

/* 44: prof — profiling, not supported */
int sys_prof(void)
{
	return -ENOSYS;
}

/* 53: lock — obsolete file locking */
int sys_lock(void)
{
	return -ENOSYS;
}
