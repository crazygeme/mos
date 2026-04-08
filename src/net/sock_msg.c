/*
 * sock_msg.c — sendmsg, recvmsg, ancillary data (cmsg), and strace helpers.
 */
#include <net/sock.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <errno.h>

#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/raw.h>
#include <lwip/ip4_addr.h>

/* ── strace-style log helpers ────────────────────────────────────────────── */

static void fmt_msg_flags(char *buf, int flags)
{
	if (flags == 0) {
		buf[0] = '0';
		buf[1] = '\0';
		return;
	}
	buf[0] = '\0';
	if (flags & MSG_DONTWAIT)
		strcat(buf, "MSG_DONTWAIT|");
	if (flags & MSG_PEEK)
		strcat(buf, "MSG_PEEK|");
	if (flags & MSG_TRUNC)
		strcat(buf, "MSG_TRUNC|");
	if (flags & MSG_OOB)
		strcat(buf, "MSG_OOB|");
	int len = (int)strlen(buf);
	if (len > 0 && buf[len - 1] == '|')
		buf[len - 1] = '\0';
}

static void fmt_sockaddr(char *buf, const struct sockaddr_in *sa)
{
	if (!sa) {
		strcpy(buf, "NULL");
		return;
	}
	const unsigned char *ip = (const unsigned char *)&sa->sin_addr.s_addr;
	unsigned port = (unsigned)(((unsigned char *)&sa->sin_port)[0] << 8 |
				   ((unsigned char *)&sa->sin_port)[1]);
	sprintf(buf,
		"{sa_family=AF_INET, sin_port=htons(%u), sin_addr=inet_addr(\"%u.%u.%u.%u\")}",
		port, ip[0], ip[1], ip[2], ip[3]);
}

static void fmt_iov(char *buf, const struct iovec *iov, size_t iovlen)
{
	if (!iov || iovlen == 0) {
		strcpy(buf, "[]");
		return;
	}
	sprintf(buf, "[{iov_base=%p, iov_len=%u}%s]", iov[0].iov_base,
		(unsigned)iov[0].iov_len, iovlen > 1 ? ", ..." : "");
}

/* ── Ancillary data (cmsg) helpers ───────────────────────────────────────── */

static void cmsg_append(struct msghdr *msg, size_t *off, int level, int type,
			const void *data, size_t dlen)
{
	size_t space = CMSG_SPACE(dlen);
	if (*off + space > (size_t)msg->msg_controllen) {
		msg->msg_flags |= MSG_CTRUNC;
		return;
	}
	struct cmsghdr *cm =
		(struct cmsghdr *)((char *)msg->msg_control + *off);
	cm->cmsg_len = CMSG_LEN(dlen);
	cm->cmsg_level = level;
	cm->cmsg_type = type;
	memcpy(CMSG_DATA(cm), data, dlen);
	*off += space;
}

static void cmsg_write(struct msghdr *msg, const mos_sock *sk)
{
	if (!msg->msg_control || !msg->msg_controllen)
		return;

	size_t off = 0;

	if (sk->cmsg_flags & SOCK_CMSG_TIMESTAMP)
		cmsg_append(msg, &off, SOL_SOCKET, SO_TIMESTAMP, &sk->rx_stamp,
			    sizeof(sk->rx_stamp));

	if (sk->cmsg_flags & SOCK_CMSG_TTL)
		cmsg_append(msg, &off, IPPROTO_IP, IP_TTL, &sk->rx_ttl,
			    sizeof(int));

	if (sk->cmsg_flags & SOCK_CMSG_PKTINFO) {
		struct in_pktinfo pi;
		pi.ipi_ifindex = sk->rx_ifindex;
		pi.ipi_spec_dst = sk->rx_dst;
		pi.ipi_addr = sk->rx_dst;
		cmsg_append(msg, &off, IPPROTO_IP, IP_PKTINFO, &pi, sizeof(pi));
	}

	msg->msg_controllen = off;
}

/* ── do_sendmsg ──────────────────────────────────────────────────────────── */

int do_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	(void)flags;
	mos_sock *sk = fd_to_sock(fd);
	int ret;

	if (!sk) {
		ret = -ENOTSOCK;
		goto log;
	}
	if (sk->err) {
		ret = sk->err;
		goto log;
	}

	size_t totlen = 0;
	size_t i;
	for (i = 0; i < msg->msg_iovlen; i++)
		totlen += msg->msg_iov[i].iov_len;

	const struct sockaddr_in *to =
		(const struct sockaddr_in *)msg->msg_name;

	if (sk->type == SOCK_STREAM) {
		if (sk->state != SS_CONNECTED &&
		    sk->state != SS_DISCONNECTING) {
			ret = -ENOTCONN;
			goto log;
		}
		size_t sent = 0;
		for (i = 0; i < msg->msg_iovlen; i++) {
			err_t e = tcp_write(sk->tcp, msg->msg_iov[i].iov_base,
					    (u16_t)msg->msg_iov[i].iov_len,
					    TCP_WRITE_FLAG_COPY);
			if (e != ERR_OK) {
				ret = sent > 0 ? (int)sent : -EIO;
				goto log;
			}
			sent += msg->msg_iov[i].iov_len;
		}
		tcp_output(sk->tcp);
		ret = (int)sent;
		goto log;
	}

	/* UDP / RAW */
	pbuf_layer layer = (sk->type == SOCK_RAW) ? PBUF_IP : PBUF_TRANSPORT;
	struct pbuf *p = pbuf_alloc(layer, (u16_t)totlen, PBUF_RAM);
	if (!p) {
		ret = -ENOBUFS;
		goto log;
	}
	u16_t off = 0;
	for (i = 0; i < msg->msg_iovlen; i++) {
		memcpy((char *)p->payload + off, msg->msg_iov[i].iov_base,
		       msg->msg_iov[i].iov_len);
		off += (u16_t)msg->msg_iov[i].iov_len;
	}

	err_t e;
	if (sk->type == SOCK_RAW && sk->hdrincl) {
		pbuf_free(p);
		char *flat = zalloc(totlen);
		if (!flat) {
			ret = -ENOBUFS;
			goto log;
		}
		size_t foff = 0;
		for (i = 0; i < msg->msg_iovlen; i++) {
			memcpy(flat + foff, msg->msg_iov[i].iov_base,
			       msg->msg_iov[i].iov_len);
			foff += msg->msg_iov[i].iov_len;
		}
		ret = raw_send_hdrincl(flat, (unsigned)totlen);
		free(flat);
		goto log;
	} else if (sk->type == SOCK_RAW) {
		ip_addr_t ip;
		const struct sockaddr_in *dst = to ? to : &sk->peer;
		ip4_addr_set_u32(ip_2_ip4(&ip), dst->sin_addr.s_addr);
		e = raw_sendto(sk->raw, p, &ip);
	} else if (to) {
		ip_addr_t ip;
		ip4_addr_set_u32(ip_2_ip4(&ip), to->sin_addr.s_addr);
		u16_t port = lwip_ntohs(to->sin_port);
		e = udp_sendto(sk->udp, p, &ip, port);
	} else {
		e = udp_send(sk->udp, p);
	}
	pbuf_free(p);
	ret = e == ERR_OK ? (int)totlen : -EIO;

log:
	if (TEST_LOG(TEST_LOG_INFO)) {
		char *addr_buf = malloc(80);
		char *iov_buf = malloc(64);
		char *flag_buf = malloc(64);
		fmt_sockaddr(addr_buf,
			     (const struct sockaddr_in *)msg->msg_name);
		fmt_iov(iov_buf, msg->msg_iov, msg->msg_iovlen);
		fmt_msg_flags(flag_buf, flags);
		klog("sendmsg(%d, {msg_name=%s, msg_namelen=%d, msg_iov=%s, msg_iovlen=%u, msg_controllen=0, msg_flags=0}, %s) = %d\n",
		     fd, addr_buf, msg->msg_namelen, iov_buf,
		     (unsigned)msg->msg_iovlen, flag_buf, ret);
		free(flag_buf);
		free(iov_buf);
		free(addr_buf);
	}
	return ret;
}

/* ── do_recvmsg ──────────────────────────────────────────────────────────── */

int do_recvmsg(int fd, struct msghdr *msg, int flags)
{
	mos_sock *sk = fd_to_sock(fd);
	size_t i;
	unsigned delivered;
	if (!sk) {
		delivered = -ENOTSOCK;
		goto done;
	}

	size_t total_len = 0;
	for (i = 0; i < msg->msg_iovlen; i++)
		total_len += msg->msg_iov[i].iov_len;
	if (total_len == 0) {
		us_to_timeval(time_wall_us(), &sk->rx_stamp);
		delivered = 0;
		goto done;
	}

	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;

	if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW) {
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->err) {
				delivered = sk->err;
				goto done;
			}
			if (flags & MSG_DONTWAIT) {
				delivered = -EAGAIN;
				goto done;
			}
			if (time_now_ms() > deadline) {
				delivered = -ETIMEDOUT;
				goto done;
			}
			if (sock_wait(sk, deadline) < 0) {
				delivered = -EINTR;
				goto done;
			}
		}
		u16_t dlen;
		rx_read(sk, &dlen, sizeof(dlen));

		unsigned remaining = (unsigned)dlen;
		delivered = 0;
		for (i = 0; i < msg->msg_iovlen && remaining > 0; i++) {
			unsigned n = (unsigned)msg->msg_iov[i].iov_len <
						     remaining ?
					     (unsigned)msg->msg_iov[i].iov_len :
					     remaining;
			rx_read(sk, msg->msg_iov[i].iov_base, n);
			delivered += n;
			remaining -= n;
		}
		while (remaining--) {
			char tmp;
			rx_read(sk, &tmp, 1);
		}

		if (msg->msg_name &&
		    msg->msg_namelen >= sizeof(struct sockaddr_in)) {
			memcpy(msg->msg_name, &sk->rx_src,
			       sizeof(struct sockaddr_in));
			msg->msg_namelen = sizeof(struct sockaddr_in);
		}
		msg->msg_flags = (delivered < (unsigned)dlen) ? MSG_TRUNC : 0;
		goto done;
	}

	/* TCP: stream scatter-read */
	while (rx_used(sk) == 0) {
		if (sk->err) {
			delivered = sk->err;
			goto done;
		}
		if (sk->state == SS_DISCONNECTING) {
			delivered = 0;
			goto done;
		}
		if (sk->state == SS_UNCONNECTED) {
			delivered = -ENOTCONN;
			goto done;
		}
		if (flags & MSG_DONTWAIT) {
			delivered = -EAGAIN;
			goto done;
		}
		if (time_now_ms() > deadline) {
			delivered = -ETIMEDOUT;
			goto done;
		}
		if (sock_wait(sk, deadline) < 0) {
			delivered = -EINTR;
			goto done;
		}
	}
	delivered = 0;
	for (i = 0; i < msg->msg_iovlen; i++) {
		unsigned n = rx_read(sk, msg->msg_iov[i].iov_base,
				     (unsigned)msg->msg_iov[i].iov_len);
		delivered += n;
		if (n < (unsigned)msg->msg_iov[i].iov_len)
			break;
	}
	if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
		memcpy(msg->msg_name, &sk->peer, sizeof(struct sockaddr_in));
		msg->msg_namelen = sizeof(struct sockaddr_in);
	}
	msg->msg_flags = 0;

done:
	if (sk && (int)delivered >= 0)
		cmsg_write(msg, sk);
	else if (msg->msg_control)
		msg->msg_controllen = 0;

	if (TEST_LOG(TEST_LOG_INFO)) {
		char *addr_buf = malloc(80);
		char *iov_buf = malloc(64);
		char *flag_buf = malloc(64);
		char *rflg_buf = malloc(64);
		const struct sockaddr_in *peer =
			msg->msg_name ?
				(const struct sockaddr_in *)msg->msg_name :
				(sk ? &sk->rx_src : NULL);
		fmt_sockaddr(addr_buf, peer);
		fmt_iov(iov_buf, msg->msg_iov, msg->msg_iovlen);
		fmt_msg_flags(flag_buf, flags);
		fmt_msg_flags(rflg_buf, msg->msg_flags);
		klog("recvmsg(%d, {msg_name=%s, msg_namelen=%d, msg_iov=%s, msg_iovlen=%u, msg_controllen=%u, msg_flags=%s}, %s) = %d\n",
		     fd, addr_buf, msg->msg_namelen, iov_buf,
		     (unsigned)msg->msg_iovlen, (unsigned)msg->msg_controllen,
		     rflg_buf, flag_buf, (int)delivered);
		free(rflg_buf);
		free(flag_buf);
		free(iov_buf);
		free(addr_buf);
	}
	return (int)delivered;
}
