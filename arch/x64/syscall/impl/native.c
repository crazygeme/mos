/* Explicit AMD64 wire conversions for shared kernel services. */
#include <ps/ps.h>
#include <lib/klib.h>
#include <errno.h>
#include <syscall/impl/syscall_internal.h>
#include <fs/fcntl.h>
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
int native_stat(unsigned call, uintptr_t arg, void *buf, int dirfd, int flags)
{
	struct stat64 st;
	int ret = call == 5   ? sys_fstat64(arg, &st) :
		  call == 6   ? sys_lstat64((void *)arg, &st) :
		  call == 262 ? sys_fstatat64(dirfd, (void *)arg, &st, flags) :
				sys_stat64((void *)arg, &st);
	if (ret < 0)
		return ret;
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
	return 0;
}
int native_clock_gettime(int id, void *out)
{
	struct timespec time;
	int ret = sys_clock_gettime(id, &time);
	if (!ret)
		*(struct native_time *)out =
			(struct native_time){ time.tv_sec, time.tv_nsec };
	return ret;
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
	int ret = do_select(nfds, local[0], local[1], local[2], timep, (void *)mask);
	if (ret >= 0) {
		unsigned shared_bytes = ((unsigned)nfds + 31) / 32 * sizeof(uint32_t);
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
