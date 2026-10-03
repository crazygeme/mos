/*
 * syscall_net.c — sys_socketcall dispatcher for MOS.
 *
 * All socket logic lives in src/net/sock*.c.
 * This file contains only the syscall entry point.
 */
#include <net/sock.h>
#include <ps/ps.h>
#include <ps/impl/ps_internal.h>
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

int sys_socketcall(int call, uint32_t *args)
{
	switch (call) {
	case SYS_SOCKET: {
		unsigned type = args[1];
		int fd;
		if (type & ~(MOS_SOCK_TYPE_MASK | MOS_SOCK_NONBLOCK |
			     MOS_SOCK_CLOEXEC))
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

	case SYS_BIND:
		return do_bind((int)(uintptr_t)args[0],
			       (const struct sockaddr *)(uintptr_t)args[1],
			       (unsigned)args[2]);

	case SYS_CONNECT:
		return do_connect((int)args[0],
				  (const struct sockaddr *)(uintptr_t)args[1],
				  (unsigned)args[2]);

	case SYS_LISTEN:
		return do_listen((int)args[0], (int)args[1]);

	case SYS_ACCEPT:
	case SYS_ACCEPT4:
		return do_accept((int)(uintptr_t)args[0],
				 (struct sockaddr *)(uintptr_t)args[1],
				 (unsigned *)(uintptr_t)args[2]);

	case SYS_GETSOCKNAME:
		return do_getsockname((int)(uintptr_t)args[0],
				      (struct sockaddr *)(uintptr_t)args[1],
				      (unsigned *)(uintptr_t)args[2]);

	case SYS_GETPEERNAME:
		return do_getpeername((int)(uintptr_t)args[0],
				      (struct sockaddr *)(uintptr_t)args[1],
				      (unsigned *)(uintptr_t)args[2]);

	case SYS_SOCKETPAIR: {
		unsigned type = args[1];
		int pair[2];
		int ret, i;
		if (!args[3])
			return -EFAULT;
		if (type & ~(MOS_SOCK_TYPE_MASK | MOS_SOCK_NONBLOCK |
			     MOS_SOCK_CLOEXEC))
			return -EINVAL;
		ret = do_socketpair((int)args[0], type & MOS_SOCK_TYPE_MASK,
				    (int)args[2], pair);
		if (ret < 0)
			return ret;
		for (i = 0; i < 2; i++) {
			if (type & MOS_SOCK_NONBLOCK)
				CURRENT_TASK()->fds[pair[i]]->f_flag |=
					O_NONBLOCK;
			if (type & MOS_SOCK_CLOEXEC)
				fd_bitmap_set(CURRENT_TASK()->fd_cloexec,
					      pair[i]);
		}
		ret = ps_write_process_memory(CURRENT_TASK(),
					      (void *)(uintptr_t)args[3], pair,
					      sizeof(pair));
		if (ret < 0) {
			fs_close(pair[0]);
			fs_close(pair[1]);
			return -EFAULT;
		}
		return 0;
	}

	case SYS_SEND:
		return do_send((int)(uintptr_t)args[0],
			       (const void *)(uintptr_t)args[1],
			       (unsigned)args[2], (int)args[3]);

	case SYS_RECV:
		return do_recv((int)(uintptr_t)args[0],
			       (void *)(uintptr_t)args[1],
			       (unsigned)(uintptr_t)args[2], (int)args[3]);

	case SYS_SENDTO:
		return do_sendto((int)(uintptr_t)args[0],
				 (const void *)(uintptr_t)args[1],
				 (unsigned)args[2], (int)args[3],
				 (const struct sockaddr_in *)(uintptr_t)args[4],
				 (unsigned)args[5]);

	case SYS_RECVFROM:
		return do_recvfrom((int)(uintptr_t)args[0],
				   (void *)(uintptr_t)args[1],
				   (unsigned)args[2], (int)args[3],
				   (struct sockaddr_in *)(uintptr_t)args[4],
				   (unsigned *)(uintptr_t)args[5]);

	case SYS_SHUTDOWN:
		return do_shutdown((int)args[0], (int)args[1]);

	case SYS_SETSOCKOPT:
		return do_setsockopt((int)args[0], (int)args[1], (int)args[2],
				     (const void *)(uintptr_t)args[3],
				     (unsigned)(uintptr_t)args[4]);

	case SYS_GETSOCKOPT:
		return do_getsockopt((int)args[0], (int)args[1], (int)args[2],
				     (void *)(uintptr_t)args[3],
				     (unsigned *)(uintptr_t)args[4]);

	case SYS_SENDMSG:
		return compat_sendmsg((int)(uintptr_t)args[0],
				      (const struct msghdr *)(uintptr_t)args[1],
				      (int)args[2]);

	case SYS_RECVMSG:
		return compat_recvmsg((int)(uintptr_t)args[0],
				      (struct msghdr *)(uintptr_t)args[1],
				      (int)args[2]);

	default:
		return -ENOSYS;
	}
}
