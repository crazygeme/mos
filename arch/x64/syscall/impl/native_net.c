/* Native AMD64 socket entry points use full-width pointers. */
#include <net/sock.h>
#include <ps/ps.h>
#include <fs/fcntl.h>
#include <errno.h>
#include "native.h"

extern int sys_socketcall(int, uint32_t *);

#define NATIVE_SOCK_TYPE_MASK 0xf
#define NATIVE_SOCK_NONBLOCK 0x800
#define NATIVE_SOCK_CLOEXEC 0x80000

static void native_socket_flags(int fd, unsigned flags)
{
	if (flags & NATIVE_SOCK_NONBLOCK)
		current->files->fds[fd]->f_flag |= O_NONBLOCK;
	if (flags & NATIVE_SOCK_CLOEXEC)
		fd_bitmap_set(current->files->cloexec, fd);
}

int native_socketpair(int domain, unsigned type, int protocol, int *out)
{
	int pair[2];
	if (!out)
		return -EFAULT;
	if (type & ~(NATIVE_SOCK_TYPE_MASK | NATIVE_SOCK_NONBLOCK |
		     NATIVE_SOCK_CLOEXEC))
		return -EINVAL;
	int ret = do_socketpair(domain, type & NATIVE_SOCK_TYPE_MASK, protocol,
				pair);
	if (ret < 0)
		return ret;
	for (unsigned i = 0; i < 2; i++)
		native_socket_flags(pair[i], type);
	ret = ps_write_process_memory(current, out, pair, sizeof(pair));
	if (ret < 0) {
		fs_close(pair[0]);
		fs_close(pair[1]);
		return -EFAULT;
	}
	return 0;
}

int native_accept4(int fd, struct sockaddr *address, unsigned *length,
		   unsigned flags)
{
	if (flags & ~(NATIVE_SOCK_NONBLOCK | NATIVE_SOCK_CLOEXEC))
		return -EINVAL;
	int accepted = do_accept(fd, address, length);
	if (accepted >= 0)
		native_socket_flags(accepted, flags);
	return accepted;
}

int native_sendto(int fd, const void *buffer, size_t length, int flags,
		  const void *address, unsigned address_length)
{
	struct iovec iov = { .iov_base = (void *)buffer, .iov_len = length };
	struct msghdr msg = { .msg_name = (void *)address,
			      .msg_namelen = address_length,
			      .msg_iov = &iov,
			      .msg_iovlen = 1 };
	return do_sendmsg(fd, &msg, flags);
}

int native_socket(int domain, unsigned type, int protocol)
{
	uint32_t args[] = { domain, type, protocol };
	return sys_socketcall(SYS_SOCKET, args);
}

struct native_socket_timeval {
	int64_t seconds, microseconds;
};

static int native_socket_timeout_option(int level, int option)
{
	return level == SOL_SOCKET &&
	       (option == SO_RCVTIMEO || option == SO_SNDTIMEO);
}

int native_setsockopt(int fd, int level, int option, const void *input,
		      unsigned length)
{
	struct native_socket_timeval wire;
	struct timeval value;
	uint64_t ms;

	if (!native_socket_timeout_option(level, option))
		return do_setsockopt(fd, level, option, input, length);
	if (length < sizeof(wire))
		return -EINVAL;
	if (ps_read_process_memory(current, input, &wire, sizeof(wire)) < 0)
		return -EFAULT;
	if (wire.seconds < 0 || wire.microseconds < 0 ||
	    wire.microseconds >= 1000000)
		return -EINVAL;
	/* The shared socket deadline is bounded to unsigned milliseconds. */
	ms = (uint64_t)wire.seconds > 0xffffffffULL / 1000ULL ?
		     0xffffffffULL :
		     (uint64_t)wire.seconds * 1000ULL +
			     ((uint64_t)wire.microseconds + 999ULL) / 1000ULL;
	if (ms > 0xffffffffULL)
		ms = 0xffffffffULL;
	value.tv_sec = ms / 1000ULL;
	value.tv_usec = (ms % 1000ULL) * 1000ULL;
	return do_setsockopt(fd, level, option, &value, sizeof(value));
}

int native_getsockopt(int fd, int level, int option, void *output,
		      unsigned *length)
{
	struct native_socket_timeval wire;
	struct timeval value;
	unsigned capacity, size = sizeof(value);
	int ret;

	if (!native_socket_timeout_option(level, option))
		return do_getsockopt(fd, level, option, output, length);
	if (!output || ps_read_process_memory(current, length, &capacity,
					      sizeof(capacity)) < 0)
		return -EFAULT;
	ret = do_getsockopt(fd, level, option, &value, &size);
	if (ret)
		return ret;
	wire.seconds = value.tv_sec;
	wire.microseconds = value.tv_usec;
	size = sizeof(wire);
	if (capacity > size)
		capacity = size;
	if ((capacity &&
	     ps_write_process_memory(current, output, &wire, capacity) < 0) ||
	    ps_write_process_memory(current, length, &size, sizeof(size)) < 0)
		return -EFAULT;
	return 0;
}
