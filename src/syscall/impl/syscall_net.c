/*
 * syscall_net.c — sys_socketcall dispatcher for MOS.
 *
 * All socket logic lives in src/net/sock*.c.
 * This file contains only the syscall entry point.
 */
#include <net/sock.h>
#include <ps/ps.h>
#include <syscall/syscall.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <errno.h>
#include <arch/abi/i386/compat.h>

int sys_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	return do_sendmsg(fd, msg, flags);
}

int sys_recvmsg(int fd, struct msghdr *msg, int flags)
{
	return do_recvmsg(fd, msg, flags);
}

#define MOS_SOCK_TYPE_MASK 0xf
#define MOS_SOCK_NONBLOCK 0x800
#define MOS_SOCK_CLOEXEC 0x80000

static int socketcall_socket(uint32_t *args)
{
	unsigned type = args[1];
	int fd;
	if (type & ~(MOS_SOCK_TYPE_MASK | MOS_SOCK_NONBLOCK | MOS_SOCK_CLOEXEC))
		return -EINVAL;
	fd = do_socket((int)args[0], (int)(type & MOS_SOCK_TYPE_MASK),
		       (int)args[2]);
	if (fd < 0)
		return fd;
	if (type & MOS_SOCK_NONBLOCK)
		CURRENT_TASK()->fds[fd]->f_flag |= O_NONBLOCK;
	if (type & MOS_SOCK_CLOEXEC)
		fd_bitmap_set(CURRENT_TASK()->fd_cloexec, fd);
	return fd;
}

static int socketcall_bind(uint32_t *args)
{
	return do_bind((int)(uintptr_t)args[0],
		       (const struct sockaddr *)(uintptr_t)args[1],
		       (unsigned)args[2]);
}

static int socketcall_connect(uint32_t *args)
{
	return do_connect((int)args[0],
			  (const struct sockaddr *)(uintptr_t)args[1],
			  (unsigned)args[2]);
}

static int socketcall_listen(uint32_t *args)
{
	return do_listen((int)args[0], (int)args[1]);
}

static int socketcall_accept4(uint32_t *args)
{
	return do_accept((int)(uintptr_t)args[0],
			 (struct sockaddr *)(uintptr_t)args[1],
			 (unsigned *)(uintptr_t)args[2]);
}

static int socketcall_getsockname(uint32_t *args)
{
	return do_getsockname((int)(uintptr_t)args[0],
			      (struct sockaddr *)(uintptr_t)args[1],
			      (unsigned *)(uintptr_t)args[2]);
}

static int socketcall_getpeername(uint32_t *args)
{
	return do_getpeername((int)(uintptr_t)args[0],
			      (struct sockaddr *)(uintptr_t)args[1],
			      (unsigned *)(uintptr_t)args[2]);
}

static int socketcall_socketpair(uint32_t *args)
{
	unsigned type = args[1];
	int pair[2];
	int ret, i;
	if (!args[3])
		return -EFAULT;
	if (type & ~(MOS_SOCK_TYPE_MASK | MOS_SOCK_NONBLOCK | MOS_SOCK_CLOEXEC))
		return -EINVAL;
	ret = do_socketpair((int)args[0], type & MOS_SOCK_TYPE_MASK,
			    (int)args[2], pair);
	if (ret < 0)
		return ret;
	for (i = 0; i < 2; i++) {
		if (type & MOS_SOCK_NONBLOCK)
			CURRENT_TASK()->fds[pair[i]]->f_flag |= O_NONBLOCK;
		if (type & MOS_SOCK_CLOEXEC)
			fd_bitmap_set(CURRENT_TASK()->fd_cloexec, pair[i]);
	}
	ret = ps_write_process_memory(
		CURRENT_TASK(), (void *)(uintptr_t)args[3], pair, sizeof(pair));
	if (ret < 0) {
		fs_close(pair[0]);
		fs_close(pair[1]);
		return -EFAULT;
	}
	return 0;
}

static int socketcall_send(uint32_t *args)
{
	return do_send((int)(uintptr_t)args[0],
		       (const void *)(uintptr_t)args[1], (unsigned)args[2],
		       (int)args[3]);
}

static int socketcall_recv(uint32_t *args)
{
	return do_recv((int)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
		       (unsigned)(uintptr_t)args[2], (int)args[3]);
}

static int socketcall_sendto(uint32_t *args)
{
	return do_sendto((int)(uintptr_t)args[0],
			 (const void *)(uintptr_t)args[1], (unsigned)args[2],
			 (int)args[3],
			 (const struct sockaddr_in *)(uintptr_t)args[4],
			 (unsigned)args[5]);
}

static int socketcall_recvfrom(uint32_t *args)
{
	return do_recvfrom((int)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
			   (unsigned)args[2], (int)args[3],
			   (struct sockaddr_in *)(uintptr_t)args[4],
			   (unsigned *)(uintptr_t)args[5]);
}

static int socketcall_shutdown(uint32_t *args)
{
	return do_shutdown((int)args[0], (int)args[1]);
}

static int socketcall_setsockopt(uint32_t *args)
{
	return do_setsockopt((int)args[0], (int)args[1], (int)args[2],
			     (const void *)(uintptr_t)args[3],
			     (unsigned)(uintptr_t)args[4]);
}

static int socketcall_getsockopt(uint32_t *args)
{
	return do_getsockopt((int)args[0], (int)args[1], (int)args[2],
			     (void *)(uintptr_t)args[3],
			     (unsigned *)(uintptr_t)args[4]);
}

static int socketcall_sendmsg(uint32_t *args)
{
	return compat_sendmsg((int)(uintptr_t)args[0],
			      (const struct msghdr *)(uintptr_t)args[1],
			      (int)args[2]);
}

static int socketcall_recvmsg(uint32_t *args)
{
	return compat_recvmsg((int)(uintptr_t)args[0],
			      (struct msghdr *)(uintptr_t)args[1],
			      (int)args[2]);
}

int sys_socketcall(int call, uint32_t *args)
{
	static int (*const calls[SYS_ACCEPT4 + 1])(uint32_t *) = {
		[SYS_SOCKET] = socketcall_socket,
		[SYS_BIND] = socketcall_bind,
		[SYS_CONNECT] = socketcall_connect,
		[SYS_LISTEN] = socketcall_listen,
		[SYS_ACCEPT] = socketcall_accept4,
		[SYS_ACCEPT4] = socketcall_accept4,
		[SYS_GETSOCKNAME] = socketcall_getsockname,
		[SYS_GETPEERNAME] = socketcall_getpeername,
		[SYS_SOCKETPAIR] = socketcall_socketpair,
		[SYS_SEND] = socketcall_send,
		[SYS_RECV] = socketcall_recv,
		[SYS_SENDTO] = socketcall_sendto,
		[SYS_RECVFROM] = socketcall_recvfrom,
		[SYS_SHUTDOWN] = socketcall_shutdown,
		[SYS_SETSOCKOPT] = socketcall_setsockopt,
		[SYS_GETSOCKOPT] = socketcall_getsockopt,
		[SYS_SENDMSG] = socketcall_sendmsg,
		[SYS_RECVMSG] = socketcall_recvmsg,
	};
	if ((unsigned)call >= sizeof(calls) / sizeof(calls[0]) || !calls[call])
		return -ENOSYS;
	return calls[call](args);
}
