/* Linux route netlink snapshots and IPv4 interface configuration. */
#include <net/core.h>
#include <net/sock.h>
#include <lwip/netif.h>
#include <net/net.h>
#include <lwip/ip4_addr.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <errno.h>

struct nl_address {
	uint16_t family, pad;
	uint32_t port, groups;
};
struct nl_header {
	uint32_t length;
	uint16_t type, flags;
	uint32_t seq, port;
};
struct nl_attr {
	uint16_t length, type;
};
struct nl_link {
	uint8_t family, pad;
	uint16_t type;
	int32_t index;
	uint32_t flags, change;
};
struct nl_ip {
	uint8_t family, prefix, flags, scope;
	uint32_t index;
};
static mos_sock *netlink_sockets;
static uint32_t next_port = 0x40000000U;
#define NL_ALIGN(n) (((n) + 3U) & ~3U)

static int port_used(uint32_t port, mos_sock *self)
{
	for (mos_sock *s = netlink_sockets; s; s = s->netlink_next)
		if (s != self && s->netlink_port == port)
			return 1;
	return 0;
}

static void autobind(mos_sock *sk)
{
	if (sk->netlink_port)
		return;
	uint32_t port = current->tgid;
	while (!port || port_used(port, sk))
		port = next_port++;
	sk->netlink_port = port;
}

int netlink_socket(int type, int protocol)
{
	if ((type != SOCK_RAW && type != SOCK_DGRAM) || protocol != 0)
		return -EPROTONOSUPPORT;
	mos_sock *sk = zalloc(sizeof(*sk));
	if (!sk)
		return -ENOMEM;
	sk->domain = AF_NETLINK;
	sk->type = type;
	spinlock_init(&sk->wait_lock);
	spinlock_init(&sk->rxbuf_lock);
	list_init(&sk->waiters);
	list_init(&sk->poll_waiters);
	if (sock_alloc_rxbuf(sk, 65536) < 0) {
		sock_destroy(sk);
		return -ENOMEM;
	}
	int fd = sock_to_fd(sk);
	if (fd < 0) {
		sock_destroy(sk);
		return fd;
	}
	sk->netlink_next = netlink_sockets;
	netlink_sockets = sk;
	return fd;
}

void netlink_release(mos_sock *sk)
{
	mos_sock **p = &netlink_sockets;
	while (*p && *p != sk)
		p = &(*p)->netlink_next;
	if (*p)
		*p = sk->netlink_next;
}

int netlink_bind(mos_sock *sk, const struct sockaddr *address, unsigned length)
{
	const struct nl_address *a = (const void *)address;
	if (length < sizeof(*a) || a->family != AF_NETLINK)
		return -EINVAL;
	if (sk->netlink_port && a->port && a->port != sk->netlink_port)
		return -EINVAL;
	if (a->port && port_used(a->port, sk))
		return -EADDRINUSE;
	sk->netlink_port = a->port;
	sk->netlink_groups = a->groups;
	autobind(sk);
	return 0;
}

int netlink_sockaddr(mos_sock *sk, struct sockaddr *address, unsigned *length)
{
	struct nl_address a = { .family = AF_NETLINK,
				.port = sk->netlink_port,
				.groups = sk->netlink_groups };
	unsigned n = *length < sizeof(a) ? *length : sizeof(a);
	memcpy(address, &a, n);
	*length = sizeof(a);
	return 0;
}

static void attribute(char *buffer, unsigned *length, unsigned type,
		      const void *data, unsigned size)
{
	struct nl_attr a = { .length = sizeof(a) + size, .type = type };
	memcpy(buffer + *length, &a, sizeof(a));
	memcpy(buffer + *length + sizeof(a), data, size);
	*length += NL_ALIGN(a.length);
}

enum {
	NLMSG_ERROR = 2,
	NLMSG_DONE = 3,
	NLM_F_ACK = 4,
	NLM_F_REPLACE = 0x100,
	RTM_NEWLINK = 16,
	RTM_GETLINK = 18,
	RTM_NEWADDR = 20,
	RTM_DELADDR = 21,
	RTM_GETADDR = 22,
	IFA_ADDRESS = 1,
	IFA_LOCAL = 2,
};

static struct netif *netlink_interface(unsigned index)
{
	for (struct netif *n = netif_list; n; n = n->next)
		if (n->num + 1U == index)
			return n;
	return NULL;
}

static int netlink_change_link(const struct nl_header *request)
{
	struct nl_link link;
	if (request->length < sizeof(*request) + sizeof(link))
		return -EINVAL;
	memcpy(&link, request + 1, sizeof(link));
	struct netif *n = netlink_interface(link.index);
	if (!n)
		return -ENODEV;
	if (link.change & ~IFF_UP ||
	    request->length != sizeof(*request) + sizeof(link))
		return -EOPNOTSUPP;
	if (link.change & IFF_UP)
		net_set_interface_up(n, (link.flags & IFF_UP) != 0);
	return 0;
}

static int netlink_change_address(const struct nl_header *request)
{
	struct nl_ip ip;
	if (request->length < sizeof(*request) + sizeof(ip))
		return -EINVAL;
	memcpy(&ip, request + 1, sizeof(ip));
	if (ip.family != AF_INET)
		return -EAFNOSUPPORT;
	if (ip.prefix > 32)
		return -EINVAL;
	struct netif *n = netlink_interface(ip.index);
	if (!n)
		return -ENODEV;
	unsigned offset = sizeof(*request) + sizeof(ip);
	uint32_t address = 0, local = 0;
	int have_address = 0, have_local = 0;
	while (offset < request->length) {
		struct nl_attr attr;
		if (request->length - offset < sizeof(attr))
			return -EINVAL;
		memcpy(&attr, (const char *)request + offset, sizeof(attr));
		if (attr.length < sizeof(attr) ||
		    attr.length > request->length - offset)
			return -EINVAL;
		if (attr.type == IFA_ADDRESS || attr.type == IFA_LOCAL) {
			if (attr.length != sizeof(attr) + sizeof(address))
				return -EINVAL;
			uint32_t value;
			memcpy(&value,
			       (const char *)request + offset + sizeof(attr),
			       sizeof(value));
			if (attr.type == IFA_LOCAL) {
				local = value;
				have_local = 1;
			} else {
				address = value;
				have_address = 1;
			}
		}
		unsigned step = NL_ALIGN(attr.length);
		if (step > request->length - offset) {
			if (attr.length != request->length - offset)
				return -EINVAL;
			step = attr.length;
		}
		offset += step;
	}
	if (!have_address && !have_local)
		return -EINVAL;
	if (have_local)
		address = local;
	uint32_t configured = ip4_addr_get_u32(netif_ip4_addr(n));
	ip4_addr_t value, mask;
	ip4_addr_set_u32(&value, address);
	ip4_addr_set_u32(&mask,
			 ip.prefix ? lwip_htonl(~0U << (32 - ip.prefix)) : 0);
	if (request->type == RTM_DELADDR) {
		if (!configured || configured != address ||
		    ip4_addr_get_u32(netif_ip4_netmask(n)) !=
			    ip4_addr_get_u32(&mask))
			return -EADDRNOTAVAIL;
		ip4_addr_t empty;
		ip4_addr_set_zero(&empty);
		netif_set_addr(n, &empty, &empty, &empty);
	} else {
		if (!address)
			return -EINVAL;
		if (configured && configured != address &&
		    !(request->flags & NLM_F_REPLACE))
			return -EEXIST;
		netif_set_addr(n, &value, &mask, netif_ip4_gw(n));
	}
	return 0;
}

static int netlink_request(mos_sock *sk, const struct nl_header *input)
{
	struct nl_header request = *input;
	int mutation = request.type == RTM_NEWLINK ||
		       request.type == RTM_NEWADDR ||
		       request.type == RTM_DELADDR;
	int error = 0;
	if (mutation) {
		if (!current->user || current->user->euid != 0)
			error = -EPERM;
		else if (request.type == RTM_NEWLINK)
			error = netlink_change_link(input);
		else
			error = netlink_change_address(input);
		if (!error && !(request.flags & NLM_F_ACK))
			return 0;
	}
	autobind(sk);
	char *buffer = zalloc(8192);
	if (!buffer)
		return -ENOMEM;
	unsigned length = 0;
	if (request.type == RTM_GETLINK || request.type == RTM_GETADDR) {
		for (struct netif *n = netif_list; n; n = n->next) {
			if (length > 3500) {
				free(buffer);
				return -ENOBUFS;
			}
			unsigned start = length;
			struct nl_header h = { .type = request.type - 2,
					       .flags = 2,
					       .seq = request.seq,
					       .port = sk->netlink_port };
			length += sizeof(h);
			int loop = n->name[0] == 'l' && n->name[1] == 'o';
			char name[IFNAMSIZ];
			if (loop)
				strcpy(name, "lo");
			else
				sprintf(name, "%c%c%u", n->name[0], n->name[1],
					n->num);
			if (request.type == RTM_GETLINK) {
				struct nl_link link = {
					.type = loop ? 772 : 1,
					.index = n->num + 1,
					.change = 0xffffffffU,
					.flags =
						(loop ? IFF_LOOPBACK :
							IFF_BROADCAST |
								 IFF_MULTICAST) |
						(netif_is_up(n) ? IFF_UP : 0) |
						(netif_is_link_up(n) ?
							 IFF_RUNNING :
							 0)
				};
				memcpy(buffer + length, &link, sizeof(link));
				length += sizeof(link);
				attribute(buffer, &length, 3, name,
					  strlen(name) + 1);
				uint32_t mtu = n->mtu;
				attribute(buffer, &length, 4, &mtu,
					  sizeof(mtu));
				char mac[6] = { 0 };
				if (!loop)
					memcpy(mac, n->hwaddr, sizeof(mac));
				attribute(buffer, &length, 1, mac, sizeof(mac));
			} else {
				if (request.length > sizeof(request) &&
				    ((const unsigned char *)(input + 1))[0] !=
					    0 &&
				    ((const unsigned char *)(input + 1))[0] !=
					    AF_INET) {
					length = start;
					continue;
				}
				uint32_t address =
					ip4_addr_get_u32(netif_ip4_addr(n));
				uint32_t mask =
					ip4_addr_get_u32(netif_ip4_netmask(n));
				if (!address) {
					length = start;
					continue;
				}
				uint32_t bits = lwip_ntohl(mask);
				unsigned prefix = 0;
				while (bits & 0x80000000U) {
					prefix++;
					bits <<= 1;
				}
				struct nl_ip ip = { .family = AF_INET,
						    .prefix = prefix,
						    .scope = loop ? 254 : 0,
						    .index = n->num + 1 };
				memcpy(buffer + length, &ip, sizeof(ip));
				length += sizeof(ip);
				attribute(buffer, &length, 1, &address,
					  sizeof(address));
				attribute(buffer, &length, 2, &address,
					  sizeof(address));
				attribute(buffer, &length, 3, name,
					  strlen(name) + 1);
				if (!loop) {
					uint32_t broadcast = address | ~mask;
					attribute(buffer, &length, 4,
						  &broadcast,
						  sizeof(broadcast));
				}
			}
			h.length = length - start;
			memcpy(buffer + start, &h, sizeof(h));
		}
	}
	struct nl_header done = { .length = sizeof(done) + sizeof(int),
				  .type = NLMSG_DONE,
				  .flags = 2,
				  .seq = request.seq,
				  .port = sk->netlink_port };
	if (mutation ||
	    (request.type != RTM_GETLINK && request.type != RTM_GETADDR)) {
		done.type = NLMSG_ERROR;
		done.flags = 0;
		if (!mutation)
			error = -EOPNOTSUPP;
		done.length += error ? request.length : sizeof(request);
	}
	memcpy(buffer + length, &done, sizeof(done));
	length += sizeof(done);
	memcpy(buffer + length, &error, sizeof(error));
	length += sizeof(error);
	if (done.type == NLMSG_ERROR) {
		unsigned echoed = error ? request.length : sizeof(request);
		memcpy(buffer + length, input, echoed);
		length += echoed;
	}
	if (rx_free(sk) < length + sizeof(uint16_t)) {
		free(buffer);
		return -ENOBUFS;
	}
	uint16_t packet = length;
	rx_write(sk, &packet, sizeof(packet));
	rx_write(sk, buffer, length);
	free(buffer);
	sock_wakeup(sk);
	return 0;
}

int netlink_sendmsg(mos_sock *sk, const struct msghdr *msg, int flags)
{
	(void)flags;
	size_t total = sock_msg_iov_total_len(msg), copied = 0;
	struct nl_header request;
	if (total < sizeof(request) || total > 4096)
		return -EINVAL;
	if (msg->msg_name) {
		const struct nl_address *a = msg->msg_name;
		if (msg->msg_namelen < sizeof(*a) || a->family != AF_NETLINK)
			return -EINVAL;
		if (a->port)
			return -ECONNREFUSED;
	}
	char *input = malloc(total);
	if (!input)
		return -ENOMEM;
	for (size_t i = 0; i < msg->msg_iovlen; i++) {
		memcpy(input + copied, msg->msg_iov[i].iov_base,
		       msg->msg_iov[i].iov_len);
		copied += msg->msg_iov[i].iov_len;
	}
	int result = -EINVAL;
	for (unsigned offset = 0; offset < total;) {
		unsigned remaining = total - offset;
		if (remaining < sizeof(request))
			goto out;
		memcpy(&request, input + offset, sizeof(request));
		if (request.length < sizeof(request) ||
		    request.length > remaining)
			goto out;
		offset += request.length == remaining ?
				  remaining :
				  NL_ALIGN(request.length);
		if (offset > total)
			goto out;
	}
	for (unsigned offset = 0; offset < total;) {
		memcpy(&request, input + offset, sizeof(request));
		result = netlink_request(
			sk, (const struct nl_header *)(input + offset));
		if (result < 0)
			goto out;
		offset += request.length == total - offset ?
				  request.length :
				  NL_ALIGN(request.length);
	}
	result = total;
out:
	free(input);
	return result;
}

int netlink_recvmsg(mos_sock *sk, struct msghdr *msg, int flags)
{
	while (rx_used(sk) < sizeof(uint16_t)) {
		if (flags & MSG_DONTWAIT)
			return -EAGAIN;
		if (sock_wait(sk, sock_recv_deadline(sk)) < 0)
			return -EINTR;
	}
	unsigned saved = sk->rx_head, delivered = 0;
	uint16_t packet;
	rx_read(sk, &packet, sizeof(packet));
	for (size_t i = 0; i < msg->msg_iovlen && delivered < packet; i++) {
		size_t n = msg->msg_iov[i].iov_len;
		if (n > packet - delivered)
			n = packet - delivered;
		rx_read(sk, msg->msg_iov[i].iov_base, n);
		delivered += n;
	}
	rx_discard(sk, packet - delivered);
	if (flags & MSG_PEEK)
		sk->rx_head = saved;
	msg->msg_flags = delivered < packet ? MSG_TRUNC : 0;
	msg->msg_controllen = 0;
	if (msg->msg_name) {
		struct nl_address a = { .family = AF_NETLINK };
		unsigned n = msg->msg_namelen < sizeof(a) ? msg->msg_namelen :
							    sizeof(a);
		memcpy(msg->msg_name, &a, n);
		msg->msg_namelen = sizeof(a);
	}
	return flags & MSG_TRUNC ? packet : delivered;
}
