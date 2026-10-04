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
		current->fds[fd]->f_flag |= O_NONBLOCK;
	if (flags & NATIVE_SOCK_CLOEXEC)
		fd_bitmap_set(current->fd_cloexec, fd);
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
