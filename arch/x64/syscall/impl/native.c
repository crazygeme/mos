/* Explicit AMD64 wire conversions for shared kernel services. */
#include <ps/ps.h>
#include <lib/klib.h>
#include <errno.h>
#include <syscall/syscall.h>
#include <fs/fcntl.h>
#include <mm/phymm.h>
#include <mm/mmap.h>
#include "native.h"
#define INT_MAX 0x7fffffff
struct native_time {
	int64_t sec, fraction;
};
struct native_stat {
	uint64_t dev, ino, nlink;
	uint32_t mode, uid, gid, pad;
	uint64_t rdev;
	int64_t size, blksize, blocks;
	struct native_time atime, mtime, ctime;
	int64_t reserved[3];
};
_Static_assert(sizeof(struct native_stat) == 144, "AMD64 stat layout");
static void native_stat_copy(void *buf, const struct stat64 *source)
{
	struct stat64 st = *source;
	struct native_stat *out = buf;
	memset(out, 0, sizeof(*out));
	out->dev = st.st_dev;
	out->ino = st.st_ino;
	out->nlink = st.st_nlink;
	out->mode = st.st_mode;
	out->uid = st.st_uid;
	out->gid = st.st_gid;
	out->rdev = st.st_rdev;
	out->size = st.st_size;
	out->blksize = st.st_blksize;
	out->blocks = st.st_blocks;
	out->atime = (struct native_time){ st.st_atime, st.st_atime_nsec };
	out->mtime = (struct native_time){ st.st_mtime, st.st_mtime_nsec };
	out->ctime = (struct native_time){ st.st_ctime, st.st_ctime_nsec };
}
int native_stat_path(const char *path, void *buf)
{
	struct stat64 st;
	int ret = sys_stat64(path, &st);
	if (!ret)
		native_stat_copy(buf, &st);
	return ret;
}

int native_stat_link(const char *path, void *buf)
{
	struct stat64 st;
	int ret = sys_lstat64(path, &st);
	if (!ret)
		native_stat_copy(buf, &st);
	return ret;
}

int native_stat_fd(int fd, void *buf)
{
	struct stat64 st;
	int ret = sys_fstat64(fd, &st);
	if (!ret)
		native_stat_copy(buf, &st);
	return ret;
}

int native_stat_at(int fd, const char *path, void *buf, int flags)
{
	struct stat64 st;
	int ret = sys_fstatat64(fd, path, &st, flags);
	if (!ret)
		native_stat_copy(buf, &st);
	return ret;
}

int native_clock_gettime(int id, void *out)
{
	return sys_clock_gettime64(id, out);
}
int native_gettimeofday(void *out, struct timezone *zone)
{
	struct timeval time;
	int ret = sys_gettimeofday(out ? &time : NULL, zone);
	if (!ret && out)
		*(struct native_time *)out =
			(struct native_time){ time.tv_sec, time.tv_usec };
	return ret;
}
static int timespec32(const struct native_time *in, struct timespec *out)
{
	if (in->sec < 0 || in->sec > INT_MAX || in->fraction < 0 ||
	    in->fraction >= 1000000000)
		return -EINVAL;
	out->tv_sec = in->sec;
	out->tv_nsec = in->fraction;
	return 0;
}

static void remaining_time(struct native_time *timeout, uint64_t start,
			   const struct native_time *requested, unsigned units,
			   int result)
{
	if (!result) {
		*timeout = (struct native_time){ 0 };
		return;
	}
	uint64_t duration = requested->sec * 1000000000ULL +
			    requested->fraction * (1000000000ULL / units);
	uint64_t elapsed = (time_now_us() - start) * 1000ULL;
	uint64_t left = elapsed < duration ? duration - elapsed : 0;
	timeout->sec = left / 1000000000ULL;
	timeout->fraction = (left % 1000000000ULL) / (1000000000ULL / units);
}

static int select64(int nfds, fd_set *reads, fd_set *writes, fd_set *excepts,
		    struct native_time *timeout, const sigset_t *mask,
		    unsigned units)
{
	struct timeval time, *timep = NULL;
	struct native_time requested = { 0 };
	fd_set sets[3];
	fd_set *wire[] = { reads, writes, excepts };
	fd_set *local[] = { NULL, NULL, NULL };
	sigset_t temporary_mask;
	uint64_t start = 0;
	if (nfds < 0 || nfds > FD_SETSIZE)
		return -EINVAL;
	if (timeout) {
		requested = *timeout;
		if (requested.sec < 0 || requested.sec > INT_MAX ||
		    requested.fraction < 0 || requested.fraction >= units)
			return -EINVAL;
		uint64_t millis = (requested.fraction + units / 1000 - 1) /
				  (units / 1000);
		if (requested.sec + millis / 1000 > INT_MAX)
			return -EINVAL;
		time.tv_sec = requested.sec + millis / 1000;
		time.tv_usec = (millis % 1000) * 1000;
		timep = &time;
		start = time_now_us();
	}
	/* AMD64 rounds descriptor sets to 64-bit words; the shared service
	 * rounds to 32-bit words. Local sets preserve the caller's error path
	 * and permit clearing the entire native output word on success. */
	unsigned bytes = ((unsigned)nfds + 63) / 64 * sizeof(uint64_t);
	for (unsigned i = 0; i < 3; i++) {
		if (wire[i]) {
			memset(&sets[i], 0, sizeof(sets[i]));
			memcpy(&sets[i], wire[i], bytes);
			local[i] = &sets[i];
		}
	}
	if (mask) {
		temporary_mask = *mask & ~((1U << (SIGKILL - 1)) |
					   (1U << (SIGSTOP - 1)));
		mask = &temporary_mask;
	}
	int ret = do_select(nfds, local[0], local[1], local[2], timep,
			    (void *)mask);
	if (ret >= 0) {
		unsigned shared_bytes =
			((unsigned)nfds + 31) / 32 * sizeof(uint32_t);
		for (unsigned i = 0; i < 3; i++) {
			if (wire[i]) {
				memset(wire[i], 0, bytes);
				memcpy(wire[i], &sets[i], shared_bytes);
			}
		}
	}
	if (timeout && (ret >= 0 || ret == -EINTR))
		remaining_time(timeout, start, &requested, units, ret);
	return ret;
}

int native_select(int nfds, fd_set *reads, fd_set *writes, fd_set *excepts,
		  void *timeout)
{
	return select64(nfds, reads, writes, excepts, timeout, NULL, 1000000);
}

int native_pselect6(int nfds, fd_set *reads, fd_set *writes, fd_set *excepts,
		    void *timeout, const void *sigmask_arg)
{
	const struct {
		const sigset_t *mask;
		uint64_t size;
	} *arg = sigmask_arg;
	if (arg && arg->mask && arg->size != sizeof(uint64_t))
		return -EINVAL;
	return select64(nfds, reads, writes, excepts, timeout,
			arg ? arg->mask : NULL, 1000000000);
}

int native_ppoll(struct pollfd *fds, uintptr_t nfds, void *timeout,
		 const sigset_t *mask, uintptr_t mask_size)
{
	struct native_time requested = { 0 };
	struct timespec time, *timep = NULL;
	sigset_t temporary_mask;
	uint64_t start = 0;
	if (nfds > MAX_FD || (mask && mask_size != sizeof(uint64_t)))
		return -EINVAL;
	if (timeout) {
		requested = *(struct native_time *)timeout;
		int ret = timespec32(&requested, &time);
		if (ret)
			return ret;
		timep = &time;
		start = time_now_us();
	}
	if (mask) {
		temporary_mask = *mask & ~((1U << (SIGKILL - 1)) |
					   (1U << (SIGSTOP - 1)));
		mask = &temporary_mask;
	}
	int ret = do_ppoll(fds, nfds, timep, mask);
	if (timeout && (ret >= 0 || ret == -EINTR))
		remaining_time(timeout, start, &requested, 1000000000, ret);
	return ret;
}

int native_nanosleep(const void *req, void *rem)
{
	struct timespec request, remaining = { 0 };
	if (!req)
		return -EFAULT;
	int ret = timespec32(req, &request);
	if (ret)
		return ret;
	ret = sys_nanosleep(&request, rem ? &remaining : NULL);
	if (rem && ret == -EINTR)
		*(struct native_time *)rem =
			(struct native_time){ remaining.tv_sec,
					      remaining.tv_nsec };
	return ret;
}

int native_clock_nanosleep(int clockid, int flags, const void *req, void *rem)
{
	struct native_time now, relative;
	if (clockid != 0 && clockid != 1)
		return -EINVAL;
	if (!req)
		return -EFAULT;
	struct timespec checked;
	int ret = timespec32(req, &checked);
	if (ret)
		return ret;
	if (!(flags & 1))
		return native_nanosleep(req, rem);
	ret = native_clock_gettime(clockid, &now);
	if (ret)
		return ret;
	const struct native_time *deadline = req;
	if (deadline->sec < now.sec ||
	    (deadline->sec == now.sec && deadline->fraction <= now.fraction))
		return 0;
	relative.sec = deadline->sec - now.sec;
	relative.fraction = deadline->fraction - now.fraction;
	if (relative.fraction < 0) {
		relative.sec--;
		relative.fraction += 1000000000;
	}
	return native_nanosleep(&relative, NULL);
}
int native_futex(int *addr, int op, int value, const void *timeout, int *addr2,
		 int value3)
{
	return sys_futex_time64(addr, op, value, timeout, addr2, value3);
}
int native_rlimit(int resource, void *value, int set)
{
	return sys_prlimit64(0, resource, set ? value : NULL,
			     set ? NULL : value);
}
struct native_rusage {
	int64_t value[18];
};
static void usage64(void *out, const rusage *in)
{
	struct native_rusage *usage = out;
	/* timeval and the fourteen counters contain eighteen signed 32-bit words. */
	const int32_t *words = (const void *)in;
	for (unsigned i = 0; i < 18; i++)
		usage->value[i] = words[i];
}
int native_getrusage(int who, void *out)
{
	rusage usage;
	extern int sys_getrusage(int, rusage *);
	int ret = sys_getrusage(who, &usage);
	if (!ret)
		usage64(out, &usage);
	return ret;
}
int native_wait4(int pid, int *status, int options, void *out)
{
	rusage usage;
	int ret = sys_wait4(pid, status, options, out ? &usage : NULL);
	if (ret > 0 && out)
		usage64(out, &usage);
	return ret;
}
struct native_flock {
	int16_t type, whence;
	int32_t pad;
	int64_t start, len;
	int32_t pid, pad2;
};
int native_fcntl(int fd, int cmd, uintptr_t argument)
{
	if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
		struct native_flock *wire = (void *)argument;
		struct flock64 lock = { .l_type = wire->type,
					.l_whence = wire->whence,
					.l_start = wire->start,
					.l_len = wire->len,
					.l_pid = wire->pid };
		int converted = cmd == F_GETLK ? F_GETLK64 :
				cmd == F_SETLK ? F_SETLK64 :
						 F_SETLKW64;
		int ret = sys_fcntl64(fd, converted, (intptr_t)&lock);
		if (!ret && cmd == F_GETLK) {
			wire->type = lock.l_type;
			wire->whence = lock.l_whence;
			wire->start = lock.l_start;
			wire->len = lock.l_len;
			wire->pid = lock.l_pid;
		}
		return ret;
	}
	return sys_fcntl(fd, cmd, argument);
}

intptr_t native_lseek(int fd, int64_t offset, int whence)
{
	uint64_t result;
	int ret =
		sys_llseek(fd, (uint64_t)offset >> 32, offset, &result, whence);
	return ret < 0 ? ret : (intptr_t)result;
}

intptr_t native_mmap(vaddr_t addr, size_t size, unsigned prot, unsigned flags,
		     int fd, uint64_t offset)
{
	if (offset > 0x7fffffffffffffffULL || (offset & (PAGE_SIZE - 1)))
		return -EINVAL;
	return do_mmap(addr, size, prot, flags, fd, offset);
}

static intptr_t arch_set_gs(uintptr_t address)
{
	if (address >= MOS_NATIVE_TASK_SIZE)
		return -EPERM;
	current->tss.gs_base = address;
	ps_load_task_segments(current);
	return 0;
}

static intptr_t arch_set_fs(uintptr_t address)
{
	if (address >= MOS_NATIVE_TASK_SIZE)
		return -EPERM;
	current->tss.fs_base = address;
	ps_load_task_segments(current);
	return 0;
}

static intptr_t arch_get_fs(uintptr_t address)
{
	uint64_t value = current->tss.fs_base;
	return ps_write_process_memory(current, (void *)address, &value,
				       sizeof(value));
}

static intptr_t arch_get_gs(uintptr_t address)
{
	uint64_t value = current->tss.gs_base;
	return ps_write_process_memory(current, (void *)address, &value,
				       sizeof(value));
}

intptr_t native_arch_prctl(unsigned operation, uintptr_t address)
{
	static intptr_t (*const calls[])(uintptr_t) = {
		arch_set_gs,
		arch_set_fs,
		arch_get_fs,
		arch_get_gs,
	};
	unsigned index = operation - 0x1001;
	if (index >= sizeof(calls) / sizeof(calls[0]))
		return -EINVAL;
	return calls[index](address);
}

intptr_t native_time(void *output)
{
	unsigned seconds;
	/* The shared service returns an i386 time_t in EAX; use its output value. */
	sys_time(&seconds);
	if (output)
		*(int64_t *)output = seconds;
	return seconds;
}

int native_utime(const char *path, const void *input)
{
	const int64_t *wire = input;
	struct utimbuf value;
	if (wire) {
		if (wire[0] < 0 || wire[0] > 0xffffffffULL || wire[1] < 0 ||
		    wire[1] > 0xffffffffULL)
			return -EOVERFLOW;
		value.actime = wire[0];
		value.modtime = wire[1];
	}
	return sys_utime(path, wire ? &value : NULL);
}

static int timeval32(const struct native_time *in, struct timeval *out)
{
	if (in->sec < 0 || in->sec > INT_MAX || in->fraction < 0 ||
	    in->fraction >= 1000000)
		return -EINVAL;
	out->tv_sec = in->sec;
	out->tv_usec = in->fraction;
	return 0;
}

int native_settimeofday(const void *input, const struct timezone *zone)
{
	struct timeval value;
	if (input) {
		int ret = timeval32(input, &value);
		if (ret)
			return ret;
	}
	return sys_settimeofday(input ? &value : NULL, zone);
}

struct native_itimer {
	struct native_time interval, value;
};
_Static_assert(sizeof(struct native_itimer) == 32,
	       "AMD64 interval timer layout");

static void itimerval64(void *output, const struct itimerval *value)
{
	*(struct native_itimer *)output = (struct native_itimer){
		{ value->it_interval.tv_sec, value->it_interval.tv_usec },
		{ value->it_value.tv_sec, value->it_value.tv_usec },
	};
}

int native_getitimer(int which, void *output)
{
	struct itimerval value;
	if (!output)
		return -EFAULT;
	int ret = sys_getitimer(which, &value);
	if (!ret)
		itimerval64(output, &value);
	return ret;
}

int native_setitimer(int which, const void *input, void *output)
{
	const struct native_itimer *wire = input;
	struct itimerval value, old;
	if (wire) {
		int ret = timeval32(&wire->interval, &value.it_interval);
		if (!ret)
			ret = timeval32(&wire->value, &value.it_value);
		if (ret)
			return ret;
	}
	int ret = sys_setitimer(which, wire ? &value : NULL,
				output ? &old : NULL);
	if (!ret && output)
		itimerval64(output, &old);
	return ret;
}

static void itimerspec64(void *output, const struct mos_itimerspec *value)
{
	*(struct native_itimer *)output = (struct native_itimer){
		{ value->it_interval.tv_sec, value->it_interval.tv_nsec },
		{ value->it_value.tv_sec, value->it_value.tv_nsec },
	};
}

int native_timer_create(int clockid, const void *input, int *timerid)
{
	const struct {
		uint64_t value;
		int32_t signo, notify, tid;
		unsigned char reserved[44];
	} *wire = input;
	struct mos_sigevent event;
	if (wire)
		event = (struct mos_sigevent){ wire->value, wire->signo,
					       wire->notify, wire->tid };
	return do_timer_create(clockid, wire ? &event : NULL, timerid,
			       wire ? wire->value : 0);
}

int native_timer_gettime(int id, void *output)
{
	struct mos_itimerspec value;
	if (!output)
		return -EFAULT;
	int ret = sys_timer_gettime(id, &value);
	if (!ret)
		itimerspec64(output, &value);
	return ret;
}

int native_timer_settime(int id, int flags, const void *input, void *output)
{
	const struct native_itimer *wire = input;
	struct mos_itimerspec value, old;
	if (!wire)
		return -EFAULT;
	int ret = timespec32(&wire->interval, &value.it_interval);
	if (!ret)
		ret = timespec32(&wire->value, &value.it_value);
	if (ret)
		return ret;
	ret = sys_timer_settime(id, flags, &value, output ? &old : NULL);
	if (!ret && output)
		itimerspec64(output, &old);
	return ret;
}

int native_sched_rr_get_interval(int pid, void *output)
{
	struct timespec value;
	if (!output)
		return -EFAULT;
	int ret = sys_sched_rr_get_interval(pid, &value);
	if (!ret)
		*(struct native_time *)output =
			(struct native_time){ value.tv_sec, value.tv_nsec };
	return ret;
}

int native_sigpending(void *output, unsigned size)
{
	sigset_t value;
	if (!output)
		return -EFAULT;
	if (size != sizeof(uint64_t))
		return -EINVAL;
	int ret = sys_rt_sigpending(&value, size);
	if (!ret)
		*(uint64_t *)output = value;
	return ret;
}

int native_sigtimedwait(const sigset_t *set, void *output, const void *timeout,
			unsigned size)
{
	struct timespec value;
	uint32_t info[32];
	if (size != sizeof(uint64_t))
		return -EINVAL;
	if (timeout) {
		int ret = timespec32(timeout, &value);
		if (ret)
			return ret;
	}
	memset(info, 0, sizeof(info));
	int ret = sys_rt_sigtimedwait(set, output ? info : NULL,
				      timeout ? &value : NULL, size);
	if (ret > 0 && output) {
		uint32_t *wire = output;
		memset(output, 0, 128);
		wire[0] = ret;
		wire[1] = info[1];
		wire[2] = info[2];
		/* AMD64 aligns the siginfo payload to eight bytes. */
		wire[4] = info[3];
		wire[5] = info[4];
		if (ret == SIGRTMIN_KERNEL)
			*(uint64_t *)&wire[6] =
				current->signal->timer_signal_value;
	}
	return ret;
}

struct native_statfs {
	int64_t type, bsize;
	uint64_t blocks, bfree, bavail, files, ffree;
	int32_t fsid[2];
	int64_t namelen, frsize, flags, spare[4];
};
_Static_assert(sizeof(struct native_statfs) == 120, "AMD64 statfs layout");

static void statfs64(void *output, const struct statfs64 *value)
{
	struct native_statfs *wire = output;
	memset(wire, 0, sizeof(*wire));
	wire->type = value->f_type;
	wire->bsize = value->f_bsize;
	wire->blocks = value->f_blocks;
	wire->bfree = value->f_bfree;
	wire->bavail = value->f_bavail;
	wire->files = value->f_files;
	wire->ffree = value->f_ffree;
	memcpy(wire->fsid, value->f_fsid, sizeof(wire->fsid));
	wire->namelen = value->f_namelen;
	wire->frsize = value->f_frsize;
	wire->flags = value->f_flags;
}

int native_statfs(const char *path, void *output)
{
	struct statfs64 value;
	if (!output)
		return -EFAULT;
	int ret = sys_statfs64(path, sizeof(value), &value);
	if (!ret)
		statfs64(output, &value);
	return ret;
}

int native_fstatfs(int fd, void *output)
{
	struct statfs64 value;
	if (!output)
		return -EFAULT;
	int ret = sys_fstatfs64(fd, sizeof(value), &value);
	if (!ret)
		statfs64(output, &value);
	return ret;
}

int native_sysinfo(void *output)
{
	struct native_sysinfo {
		int64_t uptime;
		uint64_t loads[3], totalram, freeram, sharedram, bufferram,
			totalswap, freeswap;
		uint16_t procs, pad;
		uint64_t totalhigh, freehigh;
		uint32_t mem_unit;
	} *wire = output;
	struct mos_sysinfo value;
	_Static_assert(sizeof(struct native_sysinfo) == 112,
		       "AMD64 sysinfo layout");
	if (!wire)
		return -EFAULT;
	int ret = sys_sysinfo(&value);
	if (ret)
		return ret;
	memset(wire, 0, sizeof(*wire));
	wire->uptime = value.uptime;
	for (unsigned i = 0; i < 3; i++)
		wire->loads[i] = value.loads[i];
	wire->totalram = value.totalram;
	wire->freeram = value.freeram;
	wire->sharedram = value.sharedram;
	wire->bufferram = value.bufferram;
	wire->totalswap = value.totalswap;
	wire->freeswap = value.freeswap;
	wire->procs = value.procs;
	wire->totalhigh = value.totalhigh;
	wire->freehigh = value.freehigh;
	wire->mem_unit = value.mem_unit;
	return 0;
}

intptr_t native_times(void *output)
{
	struct tms value;
	long ret = sys_times(output ? &value : NULL);
	if (output) {
		int64_t *wire = output;
		wire[0] = value.tms_utime;
		wire[1] = value.tms_stime;
		wire[2] = value.tms_cutime;
		wire[3] = value.tms_cstime;
	}
	return ret;
}

int native_getdents(unsigned fd, void *output, unsigned count)
{
	return do_getdents_native(fd, output, count);
}

int native_ioctl(int fd, unsigned command, void *arg)
{
	if (command == 0x8906) {
		struct timeval stamp;
		int ret = sys_ioctl(fd, command, (char *)&stamp);
		if (!ret) {
			if (!arg)
				return -EFAULT;
			int64_t *out = arg;
			out[0] = stamp.tv_sec;
			out[1] = stamp.tv_usec;
		}
		return ret;
	}
	return sys_ioctl(fd, command, arg);
}
