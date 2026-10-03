#include <net/sock.h>
#include <lib/klib.h>
#include <errno.h>
#include <stdint.h>
#include <fs/fs.h>
struct iovec32 {
	uint32_t base, len;
};
struct msg32 {
	uint32_t name, namelen, iov, iovlen, control, controllen, flags;
};
struct cmsg32 {
	uint32_t len;
	int32_t level, type;
};
static int compat_message(int fd, struct msg32 *wire, int flags, int receive)
{
	if (!wire || wire->iovlen > 1024 || wire->controllen > 65536)
		return -EINVAL;
	struct msghdr msg = { 0 };
	msg.msg_name = (void *)(uintptr_t)wire->name;
	msg.msg_namelen = wire->namelen;
	msg.msg_iovlen = wire->iovlen;
	msg.msg_flags = wire->flags;
	msg.msg_iov = kmalloc(msg.msg_iovlen * sizeof(struct iovec));
	if (!msg.msg_iov && msg.msg_iovlen)
		return -ENOMEM;
	struct iovec32 *iov = (void *)(uintptr_t)wire->iov;
	for (unsigned i = 0; i < msg.msg_iovlen; i++) {
		msg.msg_iov[i].iov_base = (void *)(uintptr_t)iov[i].base;
		msg.msg_iov[i].iov_len = iov[i].len;
	}
	char *control = (void *)(uintptr_t)wire->control;
	unsigned capacity = wire->controllen;
	msg.msg_control = capacity ? kmalloc(capacity * 2) : NULL;
	if (capacity && !msg.msg_control) {
		kfree(msg.msg_iov);
		return -ENOMEM;
	}
	msg.msg_controllen = receive ? capacity * 2 : 0;
	int ret = -EINVAL;
	if (!receive)
		for (unsigned off = 0; off < capacity;) {
			struct cmsg32 *src = (void *)(control + off);
			if (capacity - off < sizeof(*src) ||
			    src->len < sizeof(*src) ||
			    src->len > capacity - off)
				goto done;
			unsigned len = src->len - sizeof(*src);
			struct cmsghdr *dst = (void *)((char *)msg.msg_control +
						       msg.msg_controllen);
			dst->cmsg_len = CMSG_LEN(len);
			dst->cmsg_level = src->level;
			dst->cmsg_type = src->type;
			memcpy(CMSG_DATA(dst), src + 1, len);
			msg.msg_controllen += CMSG_SPACE(len);
			off += (src->len + 3) & ~3U;
		}
	ret = receive ? do_recvmsg(fd, &msg, flags) :
			do_sendmsg(fd, &msg, flags);
	if (ret >= 0 && receive) {
		unsigned used = 0;
		for (struct cmsghdr *src = CMSG_FIRSTHDR(&msg); src;
		     src = ((char *)src + CMSG_ALIGN(src->cmsg_len) +
						    sizeof(*src) <=
					    (char *)msg.msg_control +
						    msg.msg_controllen ?
				    (void *)((char *)src +
					     CMSG_ALIGN(src->cmsg_len)) :
				    NULL)) {
			unsigned len = src->cmsg_len - CMSG_LEN(0);
			unsigned available = capacity - used;
			unsigned copied =
				available >= sizeof(struct cmsg32) ?
					available - sizeof(struct cmsg32) :
					0;
			if (copied > len)
				copied = len;
			/* Received descriptors already belong to this process. Close any that
    * cannot be represented in the smaller i386 ancillary buffer. */
			if (src->cmsg_level == SOL_SOCKET &&
			    src->cmsg_type == SCM_RIGHTS) {
				copied &= ~3U;
				int *fds = (void *)CMSG_DATA(src);
				for (unsigned i = copied / sizeof(int);
				     i < len / sizeof(int); i++)
					fs_close(fds[i]);
			}
			if (copied < len || available < sizeof(struct cmsg32))
				msg.msg_flags |= MSG_CTRUNC;
			if (available < sizeof(struct cmsg32) ||
			    (!copied && len))
				continue;
			struct cmsg32 *dst = (void *)(control + used);
			dst->len = sizeof(*dst) + copied;
			dst->level = src->cmsg_level;
			dst->type = src->cmsg_type;
			memcpy(dst + 1, CMSG_DATA(src), copied);
			unsigned total = (dst->len + 3) & ~3U;
			used += total <= available ? total : dst->len;
		}
		wire->namelen = msg.msg_namelen;
		wire->controllen = used;
		wire->flags = msg.msg_flags;
	}
done:
	kfree(msg.msg_control);
	kfree(msg.msg_iov);
	return ret;
}
int compat_sendmsg(int fd, const void *msg, int flags)
{
	return compat_message(fd, (void *)msg, flags, 0);
}
int compat_recvmsg(int fd, void *msg, int flags)
{
	return compat_message(fd, msg, flags, 1);
}

struct mmsg32 {
	struct msg32 hdr;
	uint32_t len;
};
int compat_sendmmsg(int fd, void *messages, unsigned count, int flags)
{
	if (!messages)
		return -EFAULT;
	if (!count || count > 1024)
		return -EINVAL;
	struct mmsg32 *wire = messages;
	for (unsigned i = 0; i < count; i++) {
		int ret = compat_message(fd, &wire[i].hdr, flags, 0);
		if (ret < 0)
			return i ? (int)i : ret;
		wire[i].len = ret;
	}
	return count;
}
int compat_recvmmsg(int fd, void *messages, unsigned count, int flags,
		    void *timeout)
{
	if (!messages)
		return -EFAULT;
	if (!count || count > 1024)
		return -EINVAL;
	struct mmsg32 *wire = messages;
	for (unsigned i = 0; i < count; i++) {
		int ret = compat_message(fd, &wire[i].hdr,
					 flags | (i ? MSG_DONTWAIT : 0), 1);
		if (ret < 0)
			return i ? (int)i : ret;
		wire[i].len = ret;
	}
	/* Match the common backend, which currently ignores this timeout. */
	(void)timeout;
	return count;
}
