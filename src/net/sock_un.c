/*
 * sock_un.c — AF_UNIX domain socket operations:
 *             namespace management, bind/listen/connect/accept/release.
 *
 * Named AF_UNIX sockets follow this pattern:
 *   server: socket() → bind(path) → listen() → accept() → read/write
 *   client: socket() → connect(path) → read/write
 *
 * Internally, connect() immediately creates a paired server-side mos_sock
 * and enqueues it in the listener's unix_accept_queue.  accept() dequeues
 * that socket and wraps it in a new fd.  This avoids any synchronous
 * handshake between the two tasks.
 *
 * The Unix socket namespace is a flat array protected by a mutex.  It maps
 * resolved filesystem paths to the mos_sock that bound that path.
 */
#include <net/sock.h>
#include <fs/vfs.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <mm/mm.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <hw/time.h>
#include <errno.h>
#include <macro.h>
#include <stddef.h>

/* ── Unix socket namespace ───────────────────────────────────────────── */

#define UNIX_NS_MAX 64

typedef struct {
	char path[UNIX_PATH_MAX];
	mos_sock *sk;
} unix_ns_entry;

static unix_ns_entry unix_ns[UNIX_NS_MAX];
static mutex_t unix_ns_lock;

static void unix_ns_init(void)
{
	mutex_init(&unix_ns_lock);
}
KERNEL_INIT(2, unix_ns_init);

static mos_sock *unix_ns_lookup_locked(const char *path)
{
	int i;
	for (i = 0; i < UNIX_NS_MAX; i++) {
		if (unix_ns[i].sk && strcmp(unix_ns[i].path, path) == 0)
			return unix_ns[i].sk;
	}
	return NULL;
}

static int unix_ns_register_locked(const char *path, mos_sock *sk)
{
	int i;
	for (i = 0; i < UNIX_NS_MAX; i++) {
		if (!unix_ns[i].sk) {
			strncpy(unix_ns[i].path, path, UNIX_PATH_MAX - 1);
			unix_ns[i].path[UNIX_PATH_MAX - 1] = '\0';
			unix_ns[i].sk = sk;
			return 0;
		}
	}
	return -ENFILE;
}

/* Returns 1 if found and removed, 0 if not found. */
static int unix_ns_unregister_locked(mos_sock *sk)
{
	int i;
	for (i = 0; i < UNIX_NS_MAX; i++) {
		if (unix_ns[i].sk == sk) {
			unix_ns[i].sk = NULL;
			unix_ns[i].path[0] = '\0';
			return 1;
		}
	}
	return 0;
}

/*
 * unix_ns_remove_path — remove a path from the namespace by name.
 * Called by sys_unlink when unlinking a socket file while the socket may
 * still be open.  Clears unix_path on the owning socket so unix_release
 * will not attempt a second vfs_umount.
 */
void unix_ns_remove_path(const char *path)
{
	int i;
	mutex_lock(&unix_ns_lock);
	for (i = 0; i < UNIX_NS_MAX; i++) {
		if (unix_ns[i].sk && strcmp(unix_ns[i].path, path) == 0) {
			unix_ns[i].sk->unix_path[0] = '\0';
			unix_ns[i].sk = NULL;
			unix_ns[i].path[0] = '\0';
			break;
		}
	}
	mutex_unlock(&unix_ns_lock);
}

/* ── Bind ────────────────────────────────────────────────────────────── */

int unix_bind(mos_sock *sk, const struct sockaddr_un *addr, unsigned addrlen)
{
	task_struct *cur = CURRENT_TASK();
	char *path;
	int ret;

	if (addrlen < (unsigned)(offsetof(struct sockaddr_un, sun_path) + 1))
		return -EINVAL;
	if (sk->unix_path[0])
		return -EINVAL; /* already bound */

	path = name_get();
	resolve_path(addr->sun_path, path);

	mutex_lock(&unix_ns_lock);

	if (unix_ns_lookup_locked(path)) {
		ret = -EADDRINUSE;
		goto out_unlock;
	}

	ret = vfs_mknod(cur->root, path, S_IFSOCK | 0777, 0);
	if (ret != 0)
		goto out_unlock;

	strncpy(sk->unix_path, path, UNIX_PATH_MAX - 1);
	sk->unix_path[UNIX_PATH_MAX - 1] = '\0';

	ret = unix_ns_register_locked(path, sk);
	if (ret != 0) {
		vfs_umount(cur->root, path);
		sk->unix_path[0] = '\0';
	}

out_unlock:
	mutex_unlock(&unix_ns_lock);
	name_put(path);
	return ret;
}

/* ── Listen ──────────────────────────────────────────────────────────── */

int unix_listen(mos_sock *sk, int backlog)
{
	(void)backlog;
	if (!sk->unix_path[0])
		return -EDESTADDRREQ;
	sk->state = SS_CONNECTED; /* listening state reuses SS_CONNECTED */
	return 0;
}

/* ── Connect ─────────────────────────────────────────────────────────── */

int unix_connect(mos_sock *client, const struct sockaddr_un *addr,
		 unsigned addrlen)
{
	char *path;
	mos_sock *listener, *server_sk;
	int next_tail, ret = 0;

	if (addrlen < (unsigned)(offsetof(struct sockaddr_un, sun_path) + 1))
		return -EINVAL;
	if (client->state == SS_CONNECTED)
		return -EISCONN;

	path = name_get();
	resolve_path(addr->sun_path, path);

	mutex_lock(&unix_ns_lock);

	listener = unix_ns_lookup_locked(path);
	if (!listener || listener->type != client->type) {
		ret = -ECONNREFUSED;
		goto out;
	}

	if (client->type == SOCK_DGRAM) {
		client->unix_peer = listener;
		client->state = SS_CONNECTED;
		goto out;
	}

	if (listener->state != SS_CONNECTED) {
		ret = -ECONNREFUSED;
		goto out;
	}

	next_tail = (listener->unix_accept_tail + 1) % SOCK_ACCEPT_BACKLOG;
	if (next_tail == listener->unix_accept_head) {
		ret = -ECONNREFUSED; /* backlog full */
		goto out;
	}

	server_sk = zalloc(sizeof(*server_sk));
	if (!server_sk) {
		ret = -ENOMEM;
		goto out;
	}
	server_sk->domain = AF_UNIX;
	server_sk->type = client->type;
	server_sk->state = SS_CONNECTED;
	server_sk->unix_peer = client;
	spinlock_init(&server_sk->rxbuf_lock);

	/*
	 * Copy the listener's path into server_sk so that getsockname on the
	 * accepted socket and getpeername on the client both return the
	 * well-known path.  server_sk is NOT registered in unix_ns, so
	 * unix_release will not try to unmount it.
	 */
	strncpy(server_sk->unix_path, path, UNIX_PATH_MAX - 1);

	client->unix_peer = server_sk;
	client->state = SS_CONNECTED;

	listener->unix_accept_queue[listener->unix_accept_tail] = server_sk;
	listener->unix_accept_tail = next_tail;

out:
	mutex_unlock(&unix_ns_lock);
	name_put(path);
	if (ret == 0)
		sock_wakeup(listener);
	return ret;
}

/* ── Accept ──────────────────────────────────────────────────────────── */

int unix_accept(mos_sock *listener, struct sockaddr *addr, unsigned *addrlen)
{
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	mos_sock *server_sk;
	int fd;

	while (listener->unix_accept_head == listener->unix_accept_tail) {
		if (listener->err)
			return listener->err;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		if (sock_wait(listener, deadline) < 0)
			return -EINTR;
	}

	server_sk = listener->unix_accept_queue[listener->unix_accept_head];
	listener->unix_accept_head =
		(listener->unix_accept_head + 1) % SOCK_ACCEPT_BACKLOG;

	if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_un)) {
		struct sockaddr_un *un = (struct sockaddr_un *)addr;
		un->sun_family = AF_UNIX;
		strncpy(un->sun_path,
			(server_sk->unix_peer &&
			 server_sk->unix_peer->unix_path[0]) ?
				server_sk->unix_peer->unix_path :
				"",
			UNIX_PATH_MAX - 1);
		*addrlen = sizeof(struct sockaddr_un);
	}

	fd = sock_to_fd(server_sk);
	if (fd < 0) {
		free(server_sk);
		return -ENOMEM;
	}
	return fd;
}

/* ── AF_UNIX data path ──────────────────────────────────────────────── */

void unix_drop_passfds(mos_sock *sk)
{
	while (sk->unix_passfd_head != sk->unix_passfd_tail) {
		unix_passfd_msg *msg =
			&sk->unix_passfd_queue[sk->unix_passfd_head];
		unsigned i;
		for (i = 0; i < msg->nfds; i++) {
			if (msg->files[i])
				fs_put_file(msg->files[i]);
			msg->files[i] = NULL;
		}
		msg->nfds = 0;
		sk->unix_passfd_head =
			(sk->unix_passfd_head + 1) % UNIX_PASSFD_QUEUE;
	}
}

ssize_t unix_read(file *fp, mos_sock *sk, void *buf, size_t count)
{
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	int irq;
	int nonblock = (fp->f_flag & O_NONBLOCK) != 0;
	mos_sock *peer;

	if (sk->type == SOCK_DGRAM) {
		spinlock_lock(&sk->rxbuf_lock, &irq);
		while (rx_used(sk) < sizeof(u16_t)) {
			spinlock_unlock(&sk->rxbuf_lock, irq);
			if (sk->err)
				return sk->err;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			if (sock_wait(sk, deadline) < 0)
				return -EINTR;
			spinlock_lock(&sk->rxbuf_lock, &irq);
		}
		u16_t dlen;
		rx_read(sk, &dlen, sizeof(dlen));
		unsigned n = (unsigned)count < (unsigned)dlen ?
				     (unsigned)count :
				     (unsigned)dlen;
		rx_read(sk, buf, n);
		if (n < (unsigned)dlen) {
			char tmp;
			unsigned rem = (unsigned)dlen - n;
			while (rem--)
				rx_read(sk, &tmp, 1);
		}
		peer = sk->unix_peer;
		spinlock_unlock(&sk->rxbuf_lock, irq);
		if (peer)
			sock_wakeup(peer);
		return (ssize_t)n;
	}

	spinlock_lock(&sk->rxbuf_lock, &irq);
	while (rx_used(sk) == 0) {
		spinlock_unlock(&sk->rxbuf_lock, irq);
		if (sk->err)
			return sk->err;
		if (sk->state == SS_DISCONNECTING)
			return 0;
		if (nonblock)
			return -EAGAIN;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		if (sock_wait(sk, deadline) < 0)
			return -EINTR;
		spinlock_lock(&sk->rxbuf_lock, &irq);
	}
	unsigned n = rx_read(sk, buf, (unsigned)count);
	peer = sk->unix_peer;
	spinlock_unlock(&sk->rxbuf_lock, irq);
	if (peer)
		sock_wakeup(peer);
	return (ssize_t)n;
}

ssize_t unix_write(file *fp, mos_sock *sk, const void *buf, size_t count)
{
	mos_sock *peer = sk->unix_peer;
	const char *p = buf;
	size_t done = 0;
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	int nonblock = (fp->f_flag & O_NONBLOCK) != 0;
	int irq;

	if (!peer)
		return -EPIPE;
	if (sk->type == SOCK_DGRAM) {
		spinlock_lock(&peer->rxbuf_lock, &irq);
		u16_t dlen = (u16_t)count;
		if (rx_free(peer) < sizeof(dlen) + (unsigned)count) {
			spinlock_unlock(&peer->rxbuf_lock, irq);
			return -ENOBUFS;
		}
		rx_write(peer, &dlen, sizeof(dlen));
		unsigned n = rx_write(peer, buf, (unsigned)count);
		spinlock_unlock(&peer->rxbuf_lock, irq);
		sock_wakeup(peer);
		return (ssize_t)n;
	}

	while (done < count) {
		unsigned n;

		peer = sk->unix_peer;
		if (!peer)
			return done > 0 ? (ssize_t)done : -EPIPE;

		spinlock_lock(&peer->rxbuf_lock, &irq);
		n = rx_write(peer, p + done, (unsigned)(count - done));
		spinlock_unlock(&peer->rxbuf_lock, irq);
		if (n > 0) {
			done += n;
			sock_wakeup(peer);
			continue;
		}

		if (nonblock)
			return done > 0 ? (ssize_t)done : -EAGAIN;
		if (time_now_ms() > deadline)
			return done > 0 ? (ssize_t)done : -ETIMEDOUT;
		if (sock_wait(sk, deadline) < 0)
			return done > 0 ? (ssize_t)done : -EINTR;
	}

	return (ssize_t)done;
}

static int unix_cmsg_validate_walk(const struct msghdr *msg,
				   struct cmsghdr **next)
{
	struct cmsghdr *cm = *next;
	char *base = (char *)msg->msg_control;
	char *end = base + msg->msg_controllen;

	if (!cm)
		return 0;
	if ((char *)cm + sizeof(*cm) > end)
		return -EINVAL;
	if (cm->cmsg_len < CMSG_LEN(0))
		return -EINVAL;
	if ((char *)cm + cm->cmsg_len > end)
		return -EINVAL;

	char *step = (char *)cm + CMSG_ALIGN(cm->cmsg_len);
	*next = (step + sizeof(struct cmsghdr) <= end) ?
			(struct cmsghdr *)step :
			NULL;
	return 0;
}

static int unix_cmsg_collect_files(const struct msghdr *msg, file **files,
				   unsigned *nfds_out)
{
	unsigned nfds = 0;
	struct cmsghdr *cm;
	struct cmsghdr *next;

	if (!msg->msg_control || !msg->msg_controllen) {
		*nfds_out = 0;
		return 0;
	}

	cm = CMSG_FIRSTHDR(msg);
	while (cm) {
		next = cm;
		if (unix_cmsg_validate_walk(msg, &next) < 0)
			goto err_drop;
		if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
			goto err_drop;

		size_t payload = cm->cmsg_len - CMSG_LEN(0);
		unsigned count;
		int *fds;
		unsigned i;

		if (payload == 0 || (payload % sizeof(int)) != 0)
			goto err_drop;

		count = (unsigned)(payload / sizeof(int));
		if (nfds + count > UNIX_PASSFD_MAX)
			goto err_drop;

		fds = (int *)CMSG_DATA(cm);
		for (i = 0; i < count; i++) {
			task_struct *cur = CURRENT_TASK();
			int fd = fds[i];
			file *fp;
			if (fd < 0 || fd >= MAX_FD || !cur->fds[fd])
				goto err_drop;
			fp = cur->fds[fd];
			fs_get_file(fp);
			files[nfds++] = fp;
		}

		cm = next;
	}

	*nfds_out = nfds;
	return 0;

err_drop:
	while (nfds > 0)
		fs_put_file(files[--nfds]);
	return -EINVAL;
}

static void unix_cmsg_put_files(file **files, unsigned nfds)
{
	while (nfds > 0)
		fs_put_file(files[--nfds]);
}

static int unix_queue_passfds(mos_sock *peer, file **files, unsigned nfds,
			      unsigned ready_head)
{
	int next_tail;
	unix_passfd_msg *slot;
	unsigned i;

	if (nfds == 0)
		return 0;

	next_tail = (peer->unix_passfd_tail + 1) % UNIX_PASSFD_QUEUE;
	if (next_tail == peer->unix_passfd_head)
		return -ENOBUFS;

	slot = &peer->unix_passfd_queue[peer->unix_passfd_tail];
	slot->ready_head = ready_head;
	slot->nfds = nfds;
	for (i = 0; i < nfds; i++) {
		slot->files[i] = files[i];
		files[i] = NULL;
	}
	peer->unix_passfd_tail = next_tail;
	return 0;
}

static unsigned unix_collect_ready_passfds(mos_sock *sk, file **files)
{
	unsigned nfds = 0;

	while (sk->unix_passfd_head != sk->unix_passfd_tail) {
		unix_passfd_msg *msg =
			&sk->unix_passfd_queue[sk->unix_passfd_head];
		unsigned i;

		if (msg->ready_head > sk->rx_head)
			break;
		if (nfds + msg->nfds > UNIX_PASSFD_MAX)
			break;
		for (i = 0; i < msg->nfds; i++)
			files[nfds++] = msg->files[i];
		msg->nfds = 0;
		sk->unix_passfd_head =
			(sk->unix_passfd_head + 1) % UNIX_PASSFD_QUEUE;
	}

	return nfds;
}

static void unix_cmsg_install_fds(struct msghdr *msg, file **files,
				  unsigned nfds)
{
	int fds[UNIX_PASSFD_MAX];
	unsigned i;
	unsigned installed = 0;
	size_t payload_space;
	size_t ctrl_len = msg->msg_controllen;
	unsigned fit;

	if (nfds == 0) {
		msg->msg_controllen = 0;
		return;
	}

	if (!msg->msg_control || ctrl_len < CMSG_SPACE(sizeof(int))) {
		msg->msg_flags |= MSG_CTRUNC;
		unix_cmsg_put_files(files, nfds);
		msg->msg_controllen = 0;
		return;
	}

	payload_space = ctrl_len - CMSG_SPACE(0);
	fit = (unsigned)(payload_space / sizeof(int));
	if (fit > UNIX_PASSFD_MAX)
		fit = UNIX_PASSFD_MAX;
	if (fit > nfds)
		fit = nfds;

	for (i = 0; i < fit; i++) {
		int fd = fs_install_fd(files[i], 0);
		if (fd < 0)
			break;
		fds[installed++] = fd;
		files[i] = NULL;
	}

	if (installed < nfds)
		msg->msg_flags |= MSG_CTRUNC;

	msg->msg_controllen = ctrl_len;
	if (installed > 0) {
		size_t off = 0;
		sock_msg_cmsg_append(msg, &off, SOL_SOCKET, SCM_RIGHTS, fds,
				     installed * sizeof(int));
		msg->msg_controllen = off;
	} else {
		msg->msg_controllen = 0;
	}

	for (i = installed; i < nfds; i++) {
		if (files[i])
			fs_put_file(files[i]);
	}
}

int unix_sendmsg(mos_sock *sk, const struct msghdr *msg)
{
	mos_sock *peer = sk->unix_peer;
	file *files[UNIX_PASSFD_MAX] = { NULL };
	unsigned nfds = 0;
	size_t total_len = 0;
	size_t sent = 0;
	size_t i;
	int nonblock = sock_msg_is_nonblock(msg->msg_flags);
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	int irq;
	int ret;
	int next_tail;

	if (!peer)
		return -EPIPE;

	total_len = sock_msg_iov_total_len(msg);

	ret = unix_cmsg_collect_files(msg, files, &nfds);
	if (ret < 0)
		return ret;

	spinlock_lock(&peer->rxbuf_lock, &irq);
	next_tail = (peer->unix_passfd_tail + 1) % UNIX_PASSFD_QUEUE;
	if (nfds > 0 && next_tail == peer->unix_passfd_head) {
		ret = -ENOBUFS;
		goto out_unlock;
	}

	if (sk->type == SOCK_DGRAM) {
		u16_t dlen = (u16_t)total_len;
		if (total_len > 0xffff) {
			ret = -EMSGSIZE;
			goto out_unlock;
		}
		if (rx_free(peer) < sizeof(dlen) + (unsigned)total_len) {
			ret = -ENOBUFS;
			goto out_unlock;
		}
		rx_write(peer, &dlen, sizeof(dlen));
		rx_iov_write(peer, msg->msg_iov, msg->msg_iovlen);
		ret = unix_queue_passfds(peer, files, nfds, peer->rx_tail);
		if (ret < 0)
			goto out_unlock;
		spinlock_unlock(&peer->rxbuf_lock, irq);
		sock_wakeup(peer);
		return (int)total_len;
	}

	if (nfds > 0 && total_len > SOCK_RXBUF_SIZE - 1) {
		ret = -EMSGSIZE;
		goto out_unlock;
	}

	if (nfds > 0) {
		while (rx_free(peer) < (unsigned)total_len) {
			spinlock_unlock(&peer->rxbuf_lock, irq);
			if (nonblock) {
				unix_cmsg_put_files(files, nfds);
				return -EAGAIN;
			}
			if (time_now_ms() > deadline) {
				unix_cmsg_put_files(files, nfds);
				return -ETIMEDOUT;
			}
			if (sock_wait(sk, deadline) < 0) {
				unix_cmsg_put_files(files, nfds);
				return -EINTR;
			}

			peer = sk->unix_peer;
			if (!peer) {
				unix_cmsg_put_files(files, nfds);
				return -EPIPE;
			}
			spinlock_lock(&peer->rxbuf_lock, &irq);
		}
	}

	for (i = 0; i < msg->msg_iovlen; i++) {
		const char *base = (const char *)msg->msg_iov[i].iov_base;
		size_t left = msg->msg_iov[i].iov_len;

		while (left > 0) {
			unsigned written =
				rx_write(peer, base + (msg->msg_iov[i].iov_len - left),
					 (unsigned)left);
			if (written > 0) {
				left -= written;
				sent += written;
				continue;
			}

			spinlock_unlock(&peer->rxbuf_lock, irq);
			sock_wakeup(peer);
			if (nonblock) {
				unix_cmsg_put_files(files, nfds);
				return sent > 0 ? (int)sent : -EAGAIN;
			}
			if (time_now_ms() > deadline) {
				unix_cmsg_put_files(files, nfds);
				return sent > 0 ? (int)sent : -ETIMEDOUT;
			}
			if (sock_wait(sk, deadline) < 0) {
				unix_cmsg_put_files(files, nfds);
				return sent > 0 ? (int)sent : -EINTR;
			}

			peer = sk->unix_peer;
			if (!peer) {
				unix_cmsg_put_files(files, nfds);
				return sent > 0 ? (int)sent : -EPIPE;
			}
			spinlock_lock(&peer->rxbuf_lock, &irq);
		}
	}

	ret = unix_queue_passfds(peer, files, nfds, peer->rx_tail);
	if (ret < 0)
		goto out_unlock;
	spinlock_unlock(&peer->rxbuf_lock, irq);
	sock_wakeup(peer);
	return (int)sent;

out_unlock:
	spinlock_unlock(&peer->rxbuf_lock, irq);
	unix_cmsg_put_files(files, nfds);
	return ret;
}

int unix_recvmsg(mos_sock *sk, struct msghdr *msg, int flags)
{
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	file *files[UNIX_PASSFD_MAX] = { NULL };
	unsigned nfds = 0;
	unsigned delivered = 0;
	int irq;
	mos_sock *peer;

	if (sk->type == SOCK_DGRAM) {
		spinlock_lock(&sk->rxbuf_lock, &irq);
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->unix_passfd_head != sk->unix_passfd_tail)
				break;
			spinlock_unlock(&sk->rxbuf_lock, irq);
			if (sk->err)
				return sk->err;
			if (sock_msg_is_nonblock(flags))
				return -EAGAIN;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			if (sock_wait(sk, deadline) < 0)
				return -EINTR;
			spinlock_lock(&sk->rxbuf_lock, &irq);
		}

		if (rx_used(sk) >= sizeof(u16_t)) {
			u16_t dlen;
			unsigned remaining;

			rx_read(sk, &dlen, sizeof(dlen));
			delivered = rx_iov_read(sk, msg->msg_iov,
						msg->msg_iovlen,
						(unsigned)dlen);
			remaining = (unsigned)dlen - delivered;
			rx_discard(sk, remaining);
			msg->msg_flags =
				(delivered < (unsigned)dlen) ? MSG_TRUNC : 0;
		}

		nfds = unix_collect_ready_passfds(sk, files);
		peer = sk->unix_peer;
		spinlock_unlock(&sk->rxbuf_lock, irq);

		if (peer)
			sock_wakeup(peer);

		if (msg->msg_name &&
		    msg->msg_namelen >= sizeof(struct sockaddr_un)) {
			struct sockaddr_un *un =
				(struct sockaddr_un *)msg->msg_name;
			un->sun_family = AF_UNIX;
			strncpy(un->sun_path, peer ? peer->unix_path : "",
				UNIX_PATH_MAX - 1);
			un->sun_path[UNIX_PATH_MAX - 1] = '\0';
			msg->msg_namelen = sizeof(*un);
		}

		unix_cmsg_install_fds(msg, files, nfds);
		return (int)delivered;
	}

	spinlock_lock(&sk->rxbuf_lock, &irq);
	while (rx_used(sk) == 0 &&
	       sk->unix_passfd_head == sk->unix_passfd_tail) {
		spinlock_unlock(&sk->rxbuf_lock, irq);
		if (sk->err)
			return sk->err;
		if (sk->state == SS_DISCONNECTING)
			return 0;
		if (sk->state == SS_UNCONNECTED)
			return -ENOTCONN;
		if (sock_msg_is_nonblock(flags))
			return -EAGAIN;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		if (sock_wait(sk, deadline) < 0)
			return -EINTR;
		spinlock_lock(&sk->rxbuf_lock, &irq);
	}

	delivered = rx_iov_read(sk, msg->msg_iov, msg->msg_iovlen, rx_used(sk));
	nfds = unix_collect_ready_passfds(sk, files);
	peer = sk->unix_peer;
	spinlock_unlock(&sk->rxbuf_lock, irq);

	if (peer)
		sock_wakeup(peer);

	if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_un)) {
		struct sockaddr_un *un = (struct sockaddr_un *)msg->msg_name;
		un->sun_family = AF_UNIX;
		strncpy(un->sun_path, peer ? peer->unix_path : "",
			UNIX_PATH_MAX - 1);
		un->sun_path[UNIX_PATH_MAX - 1] = '\0';
		msg->msg_namelen = sizeof(*un);
	}
	msg->msg_flags = 0;
	unix_cmsg_install_fds(msg, files, nfds);
	return (int)delivered;
}

/* ── Release ─────────────────────────────────────────────────────────── */

/*
 * unix_release — called by sock_release when an AF_UNIX socket is closed.
 *
 * Handles three cleanup steps:
 *  1. Disconnect the peer socket (notify it of EOF).
 *  2. If this socket was bound, remove it from the namespace and unmount
 *     the VFS socket file.  Sockets created by accept() carry a copy of
 *     the listener's path but are NOT registered in unix_ns, so
 *     unix_ns_unregister_locked returns 0 and vfs_umount is skipped.
 *  3. Drain the accept queue: free unclaimed server sockets and disconnect
 *     their waiting client peers.
 */
void unix_release(mos_sock *sk)
{
	task_struct *cur = CURRENT_TASK();
	mos_sock *peer = sk->unix_peer;

	if (peer && sk->type == SOCK_STREAM) {
		peer->unix_peer = NULL;
		peer->state = SS_DISCONNECTING;
		sock_wakeup(peer);
	}

	if (sk->unix_path[0]) {
		int was_registered;
		mutex_lock(&unix_ns_lock);
		was_registered = unix_ns_unregister_locked(sk);
		mutex_unlock(&unix_ns_lock);
		if (was_registered)
			vfs_umount(cur->root, sk->unix_path);
	}

	while (sk->unix_accept_head != sk->unix_accept_tail) {
		mos_sock *pending = sk->unix_accept_queue[sk->unix_accept_head];
		sk->unix_accept_head =
			(sk->unix_accept_head + 1) % SOCK_ACCEPT_BACKLOG;
		if (pending->unix_peer) {
			pending->unix_peer->unix_peer = NULL;
			pending->unix_peer->state = SS_DISCONNECTING;
			sock_wakeup(pending->unix_peer);
		}
		unix_drop_passfds(pending);
		free(pending);
	}
}

/* ── do_socketpair ───────────────────────────────────────────────────────── */

int do_socketpair(int domain, int type, int protocol, int sv[2])
{
	if (domain != AF_UNIX)
		return -EAFNOSUPPORT;
	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return -EPROTONOSUPPORT;
	if (protocol != 0)
		return -EPROTONOSUPPORT;

	mos_sock *a = zalloc(sizeof(*a));
	mos_sock *b = zalloc(sizeof(*b));
	if (!a || !b) {
		free(a);
		free(b);
		return -ENOMEM;
	}

	a->domain = b->domain = domain;
	a->type = b->type = type;
	a->protocol = b->protocol = 0;
	a->state = b->state = SS_CONNECTED;
	a->unix_peer = b;
	b->unix_peer = a;
	spinlock_init(&a->rxbuf_lock);
	spinlock_init(&b->rxbuf_lock);

	int fd0 = sock_to_fd(a);
	if (fd0 < 0) {
		free(a);
		free(b);
		return -ENOMEM;
	}

	int fd1 = sock_to_fd(b);
	if (fd1 < 0) {
		fs_close(fd0); /* releases a; also clears b->unix_peer */
		free(b);
		return -ENOMEM;
	}

	sv[0] = fd0;
	sv[1] = fd1;
	return 0;
}
