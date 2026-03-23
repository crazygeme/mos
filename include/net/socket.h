#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include <stdint.h>

/* ── Address families ───────────────────────────────────────────────────────── */
#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2

/* ── Socket types ───────────────────────────────────────────────────────────── */
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3

/* ── socketcall sub-call numbers (Linux i386) ───────────────────────────────── */
#define SYS_SOCKET 1
#define SYS_BIND 2
#define SYS_CONNECT 3
#define SYS_LISTEN 4
#define SYS_ACCEPT 5
#define SYS_GETSOCKNAME 6
#define SYS_GETPEERNAME 7
#define SYS_SOCKETPAIR 8
#define SYS_SEND 9
#define SYS_RECV 10
#define SYS_SENDTO 11
#define SYS_RECVFROM 12
#define SYS_SHUTDOWN 13
#define SYS_SETSOCKOPT 14
#define SYS_GETSOCKOPT 15
#define SYS_SENDMSG 16
#define SYS_RECVMSG 17
#define SYS_ACCEPT4 18

/* ── Shutdown directions ────────────────────────────────────────────────────── */
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

/* ── Address structures ─────────────────────────────────────────────────────── */
typedef uint16_t sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
	in_addr_t s_addr; /* network byte order */
};

struct sockaddr {
	sa_family_t sa_family;
	char sa_data[14];
};

struct sockaddr_in {
	sa_family_t sin_family;
	in_port_t sin_port; /* network byte order */
	struct in_addr sin_addr;
	uint8_t sin_zero[8];
};

/* ── ioctl request codes ────────────────────────────────────────────────────── */
#define FIONREAD 0x541B /* bytes available to read            */
#define FIONBIO 0x5421 /* set/clear non-blocking             */

#define SIOCGIFCONF 0x8912 /* get interface list                 */
#define SIOCGIFFLAGS 0x8913 /* get interface flags                */
#define SIOCGIFADDR 0x8915 /* get interface address              */
#define SIOCGIFDSTADDR 0x8917 /* get point-to-point address         */
#define SIOCGIFBRDADDR 0x8919 /* get broadcast address              */
#define SIOCGIFNETMASK 0x891B /* get network mask                   */
#define SIOCGIFMETRIC 0x891D /* get metric                         */
#define SIOCGIFMTU 0x8921 /* get MTU                            */
#define SIOCGIFHWADDR 0x8927 /* get hardware address               */
#define SIOCGIFINDEX 0x8933 /* get interface index                */

/* ── Interface flags (ifr_flags) ────────────────────────────────────────────── */
#define IFF_UP 0x0001
#define IFF_BROADCAST 0x0002
#define IFF_LOOPBACK 0x0008
#define IFF_RUNNING 0x0040
#define IFF_MULTICAST 0x1000

#define ARPHRD_ETHER 1 /* sa_family for Ethernet hardware address */
#define IFNAMSIZ 16

/* ── ifreq / ifconf ─────────────────────────────────────────────────────────── */
struct ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct sockaddr ifr_addr;
		struct sockaddr ifr_dstaddr;
		struct sockaddr ifr_broadaddr;
		struct sockaddr ifr_netmask;
		struct sockaddr ifr_hwaddr;
		short ifr_flags;
		int ifr_ifindex;
		int ifr_metric;
		int ifr_mtu;
	};
};

struct ifconf {
	int ifc_len;
	union {
		char *ifc_buf;
		struct ifreq *ifc_req;
	};
};

/* ── Socket state ───────────────────────────────────────────────────────────── */
#define SS_UNCONNECTED 0
#define SS_CONNECTING 1
#define SS_CONNECTED 2
#define SS_DISCONNECTING 3 /* EOF received; local side may still send */

/* ── Tuning ─────────────────────────────────────────────────────────────────── */
#define SOCK_RXBUF_SIZE (8 * 1024) /* per-socket receive ring */
#define SOCK_ACCEPT_BACKLOG 8 /* accept queue depth */
#define SOCK_TIMEOUT_MS 30000 /* blocking-op timeout (ms) */

/* ── Per-socket object ──────────────────────────────────────────────────────── */
struct tcp_pcb;
struct udp_pcb;

typedef struct _mos_sock {
	int domain;
	int type;
	int protocol;
	int state; /* SS_* */
	int err; /* pending negative errno, 0 = OK */

	union {
		struct tcp_pcb *tcp;
		struct udp_pcb *udp;
	};

	/* Circular receive buffer (TCP stream or UDP datagrams) */
	char rxbuf[SOCK_RXBUF_SIZE];
	unsigned rx_head; /* consumer offset */
	unsigned rx_tail; /* producer offset */

	/* UDP recvfrom: source of most-recently received datagram */
	struct sockaddr_in rx_src;

	/* TCP listen: queue of newly-accepted tcp_pcb pointers */
	struct tcp_pcb *accept_queue[SOCK_ACCEPT_BACKLOG];
	int accept_head;
	int accept_tail;

	struct sockaddr_in local;
	struct sockaddr_in peer;
} mos_sock;

#endif /* _NET_SOCKET_H */
