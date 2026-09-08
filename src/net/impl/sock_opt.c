/*
 * sock_opt.c — do_setsockopt / do_getsockopt for MOS sockets.
 */
#include <net/sock.h>
#include <lib/klib.h>
#include <errno.h>

#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/ip.h>

/* ── Sockopt int helpers ─────────────────────────────────────────────────── */

static int sockopt_get_int(const void *optval, unsigned optlen, int *out)
{
	if (!optval || optlen < sizeof(int))
		return -EINVAL;
	*out = *(const int *)optval;
	return 0;
}

static int sockopt_put_int(void *optval, unsigned *optlen, int val)
{
	if (!optval || !optlen)
		return -EFAULT;
	unsigned copy = *optlen < sizeof(int) ? *optlen : sizeof(int);
	__builtin_memcpy(optval, &val, copy);
	*optlen = sizeof(int);
	return 0;
}

static int sockopt_get_timeval_ms(const void *optval, unsigned optlen,
				  unsigned *out)
{
	const struct timeval *tv = (const struct timeval *)optval;
	unsigned long long ms;

	if (!optval || optlen < sizeof(*tv))
		return -EINVAL;
	if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
		return -EINVAL;

	ms = (unsigned long long)tv->tv_sec * 1000ULL +
	     ((unsigned long long)tv->tv_usec + 999ULL) / 1000ULL;
	if (ms > 0xffffffffULL)
		ms = 0xffffffffULL;
	*out = (unsigned)ms;
	return 0;
}

static int sockopt_put_timeval_ms(void *optval, unsigned *optlen, unsigned ms)
{
	struct timeval tv;
	unsigned copy;

	if (!optval || !optlen)
		return -EFAULT;
	tv.tv_sec = (int)(ms / 1000);
	tv.tv_usec = (int)((ms % 1000) * 1000);
	copy = *optlen < sizeof(tv) ? *optlen : sizeof(tv);
	__builtin_memcpy(optval, &tv, copy);
	*optlen = sizeof(tv);
	return 0;
}

/* ── do_setsockopt ───────────────────────────────────────────────────────── */

int do_setsockopt(int fd, int level, int optname, const void *optval,
		  unsigned optlen)
{
	mos_sock *sk = fd_to_sock(fd);
	int ret = 0;
	int ival = 0;

	if (!sk) {
		ret = -ENOTSOCK;
		goto done;
	}

	if (level == SOL_SOCKET) {
		switch (optname) {
		case SO_REUSEADDR:
			if (sockopt_get_int(optval, optlen, &ival) < 0) {
				ret = -EINVAL;
				goto done;
			}
			if (sk->tcp) {
				if (ival)
					ip_set_option(sk->tcp, SOF_REUSEADDR);
				else
					ip_reset_option(sk->tcp, SOF_REUSEADDR);
			} else if (sk->udp) {
				if (ival)
					ip_set_option(sk->udp, SOF_REUSEADDR);
				else
					ip_reset_option(sk->udp, SOF_REUSEADDR);
			}
			break;

		case SO_KEEPALIVE:
			if (sockopt_get_int(optval, optlen, &ival) < 0) {
				ret = -EINVAL;
				goto done;
			}
			if (sk->type != SOCK_STREAM) {
				ret = -ENOPROTOOPT;
				goto done;
			}
			if (sk->tcp) {
				if (ival)
					ip_set_option(sk->tcp, SOF_KEEPALIVE);
				else
					ip_reset_option(sk->tcp, SOF_KEEPALIVE);
			}
			break;

		case SO_BROADCAST:
			if (sockopt_get_int(optval, optlen, &ival) < 0) {
				ret = -EINVAL;
				goto done;
			}
			if (sk->udp) {
				if (ival)
					ip_set_option(sk->udp, SOF_BROADCAST);
				else
					ip_reset_option(sk->udp, SOF_BROADCAST);
			}
			break;

		case SO_RCVBUF:
		case SO_SNDBUF:
		case SO_LINGER:
			break;

		case SO_RCVTIMEO:
			ret = sockopt_get_timeval_ms(optval, optlen,
						     &sk->recv_timeout_ms);
			goto done;

		case SO_SNDTIMEO:
			ret = sockopt_get_timeval_ms(optval, optlen,
						     &sk->send_timeout_ms);
			goto done;

		case SO_ERROR:
			ret = -ENOPROTOOPT;
			goto done;

		case SO_TIMESTAMP:
			if (sockopt_get_int(optval, optlen, &ival) < 0) {
				ret = -EINVAL;
				goto done;
			}
			if (ival)
				sk->cmsg_flags |= SOCK_CMSG_TIMESTAMP;
			else
				sk->cmsg_flags &= ~SOCK_CMSG_TIMESTAMP;
			break;

		default:
			ret = -ENOPROTOOPT;
			goto done;
		}
		goto done;
	}

	if (level == IPPROTO_IP || level == IPPROTO_ICMP ||
	    level == IPPROTO_RAW) {
		switch (optname) {
		case IP_HDRINCL:
			if (sk->type != SOCK_RAW) {
				ret = -ENOPROTOOPT;
				goto done;
			}
			if (sockopt_get_int(optval, optlen, &ival) < 0) {
				ret = -EINVAL;
				goto done;
			}
			sk->hdrincl = ival ? 1 : 0;
			break;

		case IP_TTL:
			if (sockopt_get_int(optval, optlen, &ival) < 0) {
				ret = -EINVAL;
				goto done;
			}
			if (ival)
				sk->cmsg_flags |= SOCK_CMSG_TTL;
			else
				sk->cmsg_flags &= ~SOCK_CMSG_TTL;
			break;

		case IP_PKTINFO:
			if (sockopt_get_int(optval, optlen, &ival) < 0) {
				ret = -EINVAL;
				goto done;
			}
			if (ival)
				sk->cmsg_flags |= SOCK_CMSG_PKTINFO;
			else
				sk->cmsg_flags &= ~SOCK_CMSG_PKTINFO;
			break;

		case ICMP_FILTER:
			if (sk->type != SOCK_RAW ||
			    sk->protocol != IPPROTO_ICMP) {
				ret = -ENOPROTOOPT;
				goto done;
			}
			if (optlen < sizeof(struct icmp_filter) || !optval) {
				ret = -EINVAL;
				goto done;
			}
			sk->icmp_filter = *(const struct icmp_filter *)optval;
			break;

		case IP_RECVERR:
			break;

		default:
			ret = -ENOPROTOOPT;
			goto done;
		}
		goto done;
	}

	if (level == IPPROTO_TCP) {
		if (sk->type != SOCK_STREAM) {
			ret = -ENOPROTOOPT;
			goto done;
		}
		switch (optname) {
		case TCP_NODELAY:
			if (sockopt_get_int(optval, optlen, &ival) < 0) {
				ret = -EINVAL;
				goto done;
			}
			if (sk->tcp) {
				if (ival)
					tcp_nagle_disable(sk->tcp);
				else
					tcp_nagle_enable(sk->tcp);
			}
			break;

		case TCP_KEEPIDLE:
			break;

		default:
			ret = -ENOPROTOOPT;
			goto done;
		}
	}

done:
	if (TEST_LOG(TEST_LOG_TRACE) && ret == -ENOPROTOOPT)
		klog("setsockopt(%d, level=%d, optname=%d) = -ENOPROTOOPT\n",
		     fd, level, optname);
	return ret;
}

/* ── do_getsockopt ───────────────────────────────────────────────────────── */

int do_getsockopt(int fd, int level, int optname, void *optval,
		  unsigned *optlen)
{
	mos_sock *sk = fd_to_sock(fd);
	int ret = -ENOPROTOOPT;

	if (!sk) {
		ret = -ENOTSOCK;
		goto done;
	}

	if (level == SOL_SOCKET) {
		switch (optname) {
		case SO_ERROR: {
			int err = sk->err ? -sk->err : 0;
			sk->err = 0;
			ret = sockopt_put_int(optval, optlen, err);
			goto done;
		}
		case SO_TYPE:
			ret = sockopt_put_int(optval, optlen, sk->type);
			goto done;

		case SO_REUSEADDR: {
			int val = 0;
			if (sk->tcp)
				val = ip_get_option(sk->tcp, SOF_REUSEADDR) ?
					      1 :
					      0;
			else if (sk->udp)
				val = ip_get_option(sk->udp, SOF_REUSEADDR) ?
					      1 :
					      0;
			ret = sockopt_put_int(optval, optlen, val);
			goto done;
		}
		case SO_KEEPALIVE: {
			int val =
				sk->tcp ?
					(ip_get_option(sk->tcp, SOF_KEEPALIVE) ?
						 1 :
						 0) :
					0;
			ret = sockopt_put_int(optval, optlen, val);
			goto done;
		}
		case SO_RCVBUF:
			ret = sockopt_put_int(optval, optlen,
					      (int)sk->rxbuf_size);
			goto done;

		case SO_SNDBUF: {
			int val = sk->tcp ? (int)tcp_sndbuf(sk->tcp) : 0;
			ret = sockopt_put_int(optval, optlen, val);
			goto done;
		}
		case SO_BROADCAST: {
			int val =
				sk->udp ?
					(ip_get_option(sk->udp, SOF_BROADCAST) ?
						 1 :
						 0) :
					0;
			ret = sockopt_put_int(optval, optlen, val);
			goto done;
		}
		case SO_TIMESTAMP:
			ret = sockopt_put_int(
				optval, optlen,
				(sk->cmsg_flags & SOCK_CMSG_TIMESTAMP) ? 1 : 0);
			goto done;

		case SO_RCVTIMEO:
			ret = sockopt_put_timeval_ms(optval, optlen,
						     sk->recv_timeout_ms);
			goto done;

		case SO_SNDTIMEO:
			ret = sockopt_put_timeval_ms(optval, optlen,
						     sk->send_timeout_ms);
			goto done;

		default:
			goto done;
		}
	}

	if (level == IPPROTO_IP || level == IPPROTO_ICMP ||
	    level == IPPROTO_RAW) {
		switch (optname) {
		case IP_HDRINCL:
			ret = sockopt_put_int(optval, optlen, sk->hdrincl);
			goto done;

		case IP_TTL:
			ret = sockopt_put_int(
				optval, optlen,
				(sk->cmsg_flags & SOCK_CMSG_TTL) ? 1 : 0);
			goto done;

		case IP_PKTINFO:
			ret = sockopt_put_int(
				optval, optlen,
				(sk->cmsg_flags & SOCK_CMSG_PKTINFO) ? 1 : 0);
			goto done;

		case ICMP_FILTER:
			if (sk->type != SOCK_RAW ||
			    sk->protocol != IPPROTO_ICMP) {
				goto done;
			}
			if (!optval || !optlen ||
			    *optlen < sizeof(struct icmp_filter)) {
				ret = -EINVAL;
				goto done;
			}
			*(struct icmp_filter *)optval = sk->icmp_filter;
			*optlen = sizeof(struct icmp_filter);
			ret = 0;
			goto done;

		default:
			goto done;
		}
	}

	if (level == IPPROTO_TCP) {
		if (sk->type != SOCK_STREAM)
			goto done;

		switch (optname) {
		case TCP_NODELAY: {
			int val = sk->tcp ? (tcp_nagle_disabled(sk->tcp) ? 1 :
									   0) :
					    0;
			ret = sockopt_put_int(optval, optlen, val);
			goto done;
		}
		case TCP_MAXSEG: {
			int val = sk->tcp ? (int)sk->tcp->mss : 536;
			ret = sockopt_put_int(optval, optlen, val);
			goto done;
		}
		default:
			goto done;
		}
	}

done:
	if (TEST_LOG(TEST_LOG_TRACE) && ret == -ENOPROTOOPT)
		klog("getsockopt(%d, level=%d, optname=%d) = -ENOPROTOOPT\n",
		     fd, level, optname);
	return ret;
}
