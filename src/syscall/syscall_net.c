/*
 * syscall_net.c — sys_socketcall dispatcher for MOS.
 *
 * All socket logic lives in src/net/sock*.c.
 * This file contains only the syscall entry point.
 */
#include <net/sock.h>
#include <errno.h>

int sys_socketcall(int call, unsigned long *args)
{
	switch (call) {
	case SYS_SOCKET:
		return do_socket((int)args[0], (int)args[1], (int)args[2]);

	case SYS_BIND:
		return do_bind((int)args[0],
			       (const struct sockaddr_in *)args[1],
			       (unsigned)args[2]);

	case SYS_CONNECT:
		return do_connect((int)args[0],
				  (const struct sockaddr_in *)args[1],
				  (unsigned)args[2]);

	case SYS_LISTEN:
		return do_listen((int)args[0], (int)args[1]);

	case SYS_ACCEPT:
	case SYS_ACCEPT4:
		return do_accept((int)args[0], (struct sockaddr_in *)args[1],
				 (unsigned *)args[2]);

	case SYS_GETSOCKNAME:
		return do_getsockname((int)args[0],
				      (struct sockaddr_in *)args[1],
				      (unsigned *)args[2]);

	case SYS_GETPEERNAME:
		return do_getpeername((int)args[0],
				      (struct sockaddr_in *)args[1],
				      (unsigned *)args[2]);

	case SYS_SOCKETPAIR:
		return do_socketpair((int)args[0], (int)args[1], (int)args[2],
				     (int *)args[3]);

	case SYS_SEND:
		return do_send((int)args[0], (const void *)args[1],
			       (unsigned)args[2], (int)args[3]);

	case SYS_RECV:
		return do_recv((int)args[0], (void *)args[1], (unsigned)args[2],
			       (int)args[3]);

	case SYS_SENDTO:
		return do_sendto((int)args[0], (const void *)args[1],
				 (unsigned)args[2], (int)args[3],
				 (const struct sockaddr_in *)args[4],
				 (unsigned)args[5]);

	case SYS_RECVFROM:
		return do_recvfrom((int)args[0], (void *)args[1],
				   (unsigned)args[2], (int)args[3],
				   (struct sockaddr_in *)args[4],
				   (unsigned *)args[5]);

	case SYS_SHUTDOWN:
		return do_shutdown((int)args[0], (int)args[1]);

	case SYS_SETSOCKOPT:
		return do_setsockopt((int)args[0], (int)args[1], (int)args[2],
				     (const void *)args[3], (unsigned)args[4]);

	case SYS_GETSOCKOPT:
		return do_getsockopt((int)args[0], (int)args[1], (int)args[2],
				     (void *)args[3], (unsigned *)args[4]);

	case SYS_SENDMSG:
		return do_sendmsg((int)args[0], (const struct msghdr *)args[1],
				  (int)args[2]);

	case SYS_RECVMSG:
		return do_recvmsg((int)args[0], (struct msghdr *)args[1],
				  (int)args[2]);

	default:
		return -ENOSYS;
	}
}
