#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <hw/time.h>
#include <lib/lock.h>

/* ── Address families ───────────────────────────────────────────────────────── */
#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2

/* ── Socket types ───────────────────────────────────────────────────────────── */
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3

/* ── IP protocols ───────────────────────────────────────────────────────────── */
#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_RAW 255 /* raw socket with IP_HDRINCL */

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

#define UNIX_PATH_MAX 108

struct sockaddr_un {
	sa_family_t sun_family; /* AF_UNIX */
	char sun_path[UNIX_PATH_MAX]; /* pathname */
};

/* ── ioctl request codes ────────────────────────────────────────────────────── */
#define FIONREAD 0x541B /* bytes available to read            */
#define FIONBIO 0x5421 /* set/clear non-blocking             */

#define SIOCGSTAMP 0x8906 /* get timestamp of last received pkt */
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

/* ── Socket option levels (Linux i386 values) ───────────────────────────────── */
#define SOL_SOCKET 1 /* options for socket level */

/* ── SOL_SOCKET option names (Linux i386 values) ────────────────────────────── */
#define SO_REUSEADDR 2 /* allow local address reuse */
#define SO_TYPE 3 /* get socket type */
#define SO_ERROR 4 /* get error status and clear */
#define SO_SNDBUF 7 /* send buffer size */
#define SO_RCVBUF 8 /* receive buffer size */
#define SO_KEEPALIVE 9 /* keep connections alive */
#define SO_LINGER 13 /* linger on close if data present */
#define SO_RCVTIMEO 20 /* receive timeout */
#define SO_SNDTIMEO 21 /* send timeout */
#define SO_TIMESTAMP 29 /* receive SW timestamp as cmsg (struct timeval) */

/* ── IPPROTO_IP option names (Linux values) ─────────────────────────────────── */
#define IP_TTL 2 /* receive TTL as cmsg (int)                */
#define IP_HDRINCL 3 /* application provides the IP header (SOCK_RAW) */
#define IP_PKTINFO 8 /* receive pktinfo as cmsg (struct in_pktinfo) */
#define IP_RECVERR 11 /* receive ICMP errors via error queue      */

/* ── IPPROTO_ICMP option names ──────────────────────────────────────────────── */
#define ICMP_FILTER \
	1 /* set/get ICMP type filter bitmask (struct icmp_filter) */

/* ICMP_FILTER bitmask: bit N set → drop incoming ICMP messages of type N */
struct icmp_filter {
	uint32_t data;
};

/* ── IPPROTO_TCP option names (Linux values) ────────────────────────────────── */
#define TCP_NODELAY 1 /* disable Nagle algorithm */
#define TCP_MAXSEG 2 /* maximum segment size */
#define TCP_KEEPIDLE 4 /* keepalive idle time (seconds) */

/* ── linger structure (for SO_LINGER) ───────────────────────────────────────── */
struct linger {
	int l_onoff; /* linger active */
	int l_linger; /* linger time in seconds */
};

/* ── Ancillary data (cmsg) ──────────────────────────────────────────────────── */

struct cmsghdr {
	size_t cmsg_len; /* byte count including this header */
	int cmsg_level; /* protocol level */
	int cmsg_type; /* protocol-specific type */
	/* followed by unsigned char cmsg_data[] */
};

/* Destination address and incoming interface for IP_PKTINFO */
struct in_pktinfo {
	int ipi_ifindex; /* incoming interface index */
	struct in_addr ipi_spec_dst; /* local address (same as ipi_addr for rx) */
	struct in_addr ipi_addr; /* header destination address */
};

#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_DATA(cmsg) \
	((void *)((char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr))))
#define CMSG_FIRSTHDR(mhdr)                                         \
	((size_t)(mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? \
		 (struct cmsghdr *)(mhdr)->msg_control :            \
		 (struct cmsghdr *)0)

/* ── Scatter/gather I/O ─────────────────────────────────────────────────────── */
struct iovec {
	void *iov_base;
	size_t iov_len;
};

/* ── Message header (sendmsg / recvmsg) ─────────────────────────────────────── */
struct msghdr {
	void *msg_name; /* optional peer address          */
	unsigned msg_namelen; /* size of msg_name               */
	struct iovec *msg_iov; /* scatter/gather array           */
	size_t msg_iovlen; /* # of elements in msg_iov       */
	void *msg_control; /* ancillary data (unused)        */
	size_t msg_controllen; /* ancillary data length (unused) */
	int msg_flags; /* flags on received message      */
};

/* ── Message flags ───────────────────────────────────────────────────────────── */
#define MSG_OOB 0x01
#define MSG_PEEK 0x02
#define MSG_DONTROUTE 0x04
#define MSG_DONTWAIT 0x40
#define MSG_TRUNC 0x20
#define MSG_CTRUNC 0x08

/* ── Socket state ───────────────────────────────────────────────────────────── */
#define SS_UNCONNECTED 0
#define SS_CONNECTING 1
#define SS_CONNECTED 2
#define SS_DISCONNECTING 3 /* EOF received; local side may still send */

/* ── Tuning ─────────────────────────────────────────────────────────────────── */
#define SOCK_RXBUF_SIZE (8 * 1024) /* per-socket receive ring */
#define SOCK_ACCEPT_BACKLOG 8 /* accept queue depth */
#define SOCK_TIMEOUT_MS 30000 /* blocking-op timeout (ms) */

/* ── Per-socket cmsg option flags (stored in mos_sock::cmsg_flags) ─────────── */
#define SOCK_CMSG_TIMESTAMP (1u << 0) /* SO_TIMESTAMP  */
#define SOCK_CMSG_PKTINFO (1u << 1) /* IP_PKTINFO    */
#define SOCK_CMSG_TTL (1u << 2) /* IP_TTL        */

/* ── Per-socket object ──────────────────────────────────────────────────────── */
struct tcp_pcb;
struct udp_pcb;
struct raw_pcb;
struct _task_struct;

typedef struct _mos_sock {
	int domain;
	int type;
	int protocol;
	int state; /* SS_* */
	int err; /* pending negative errno, 0 = OK */

	union {
		struct tcp_pcb *tcp;
		struct udp_pcb *udp;
		struct raw_pcb *raw;
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

	/* Timestamp of the last received packet (SIOCGSTAMP / SO_TIMESTAMP) */
	struct timeval rx_stamp;

	/* IP_HDRINCL: application provides the full IP header on send */
	int hdrincl;

	/* Per-datagram metadata captured in the lwIP recv callbacks */
	struct in_addr rx_dst; /* IP_PKTINFO: destination address */
	int rx_ifindex; /* IP_PKTINFO: incoming interface index */
	int rx_ttl; /* IP_TTL: hop limit of received packet */

	/* Bitmask of enabled cmsg options (SOCK_CMSG_*) */
	unsigned int cmsg_flags;

	/* ICMP_FILTER: bitmask of ICMP types to drop (bit N → block type N) */
	struct icmp_filter icmp_filter;

	/* Task currently blocked waiting for data or state change, or NULL */
	struct _task_struct *waiter;

	/* poll/select wakeup task, woken alongside waiter wakeups */
	struct _task_struct *poll_task;

	/* AF_UNIX socketpair: pointer to the other end, or NULL if closed */
	struct _mos_sock *unix_peer;

	/*
	 * AF_UNIX named socket: filesystem path this socket is bound to.
	 * Empty string for anonymous sockets (socketpair, accepted sockets
	 * where only the server side carries the listener's name for getpeer).
	 */
	char unix_path[UNIX_PATH_MAX];

	/*
	 * AF_UNIX listen: queue of server-side mos_sock* ready for accept().
	 * Filled by unix_connect(), drained by unix_accept().
	 */
	struct _mos_sock *unix_accept_queue[SOCK_ACCEPT_BACKLOG];
	int unix_accept_head;
	int unix_accept_tail;

	/*
	 * Protects rx_head, rx_tail, and rxbuf for AF_UNIX sockets.
	 *
	 * Network sockets (AF_INET) are safe without this lock because their
	 * ring buffer follows a strict SPSC discipline: rx_write is only ever
	 * called from the NIC IRQ handler (single producer) and rx_read from
	 * a single task (single consumer).  A uniprocessor guarantee means
	 * the ISR cannot be re-entered, so no concurrent modification occurs.
	 *
	 * AF_UNIX sockets break that assumption: after fork() both the parent
	 * and the child hold a reference to the same mos_sock.  Either task
	 * can call sock_write concurrently, making rx_write a multi-producer
	 * operation.  A preemption mid-loop in rx_write would corrupt rx_tail
	 * and interleave bytes from the two callers.  The spinlock prevents
	 * that by disabling interrupts (and thus preemption) for the duration
	 * of every ring-buffer access on the AF_UNIX path.
	 */
	spinlock_t rxbuf_lock;
} mos_sock;

#endif /* _NET_SOCKET_H */
