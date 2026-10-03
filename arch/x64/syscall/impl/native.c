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
int native_stat(unsigned call, uintptr_t arg, void *buf, int dirfd)
{
	struct stat64 st;
	int ret = call == 5   ? sys_fstat64(arg, &st) :
		  call == 6   ? sys_lstat64((void *)arg, &st) :
		  call == 262 ? sys_fstatat64(dirfd, (void *)arg, &st, 0) :
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
int native_futex(int *addr, int op, int value, const void *timeout, int *addr2,
		 int value3)
{
	struct timespec time;
	unsigned cmd = op & 0x7f;
	if (timeout && (cmd == 0 || cmd == 9)) {
		int ret = timespec32(timeout, &time);
		if (ret)
			return ret;
		timeout = &time;
	}
	return sys_futex(addr, op, value, timeout, addr2, value3);
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
