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
	if (client->type != SOCK_STREAM)
		return -EOPNOTSUPP;
	if (client->state == SS_CONNECTED)
		return -EISCONN;

	path = name_get();
	resolve_path(addr->sun_path, path);

	mutex_lock(&unix_ns_lock);

	listener = unix_ns_lookup_locked(path);
	if (!listener || listener->state != SS_CONNECTED ||
	    listener->type != SOCK_STREAM) {
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

	if (peer) {
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
