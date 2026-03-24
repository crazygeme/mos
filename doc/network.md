# Network Stack

**Source:** `src/hw/nic_intel_8254x.c`, `src/hw/nic.c`, `src/net/net.c`, `src/net/sock.c`, `src/net/sock_ops.c`, `src/net/sock_cb.c`, `src/net/sock_msg.c`, `src/net/sock_opt.c`, `src/syscall/syscall_net.c`
**Headers:** `include/hw/nic.h`, `include/net/net.h`, `include/net/socket.h`, `include/net/sock.h`

---

## Overview

The network stack is built in four layers:

```
User space
  │  sys_socketcall (BSD socket API)
  ▼
syscall_net.c    ← socket operations, lwIP callbacks, rx ring buffer
  │  lwIP NO_SYS API (tcp_*, udp_*, raw_*)
  ▼
lwIP             ← TCP/IP protocol stack (NO_SYS mode)
  │  netif input/output
  ▼
net.c            ← netif glue, DHCP, lwIP timer pump
  │  nic_dev send/rx_notify
  ▼
nic_intel_8254x.c  ← Intel e1000 driver (MMIO, DMA, interrupt-driven RX)
```

lwIP runs in **NO_SYS mode** — no internal threads. All protocol processing happens synchronously inside interrupt handlers or explicit `sys_check_timeouts()` calls.

---

## 1. NIC driver — Intel 8254x (e1000)

**File:** `src/hw/nic_intel_8254x.c`
**Supported hardware:** Intel 82540EM (PCI `8086:100E`) — the QEMU default `e1000`.

### Hardware interface

Accessed via MMIO at BAR0. Key register groups:

| Register | Offset | Purpose |
|----------|--------|---------|
| `CTRL` | 0x0000 | Control (reset, link-up) |
| `ICR` / `IMS` / `IMC` | 0x00C0/D0/D8 | Interrupt cause / mask set / mask clear |
| `RCTL` | 0x0100 | Receive control (enable, buffer size, promiscuous) |
| `TCTL` / `TIPG` | 0x0400/10 | Transmit control / inter-packet gap |
| `RDBAL/H/LEN/H/T` | 0x2800… | RX descriptor ring base, length, head, tail |
| `TDBAL/H/LEN/H/T` | 0x3800… | TX descriptor ring base, length, head, tail |
| `RAL0` / `RAH0` | 0x5400/04 | Receive address (MAC) filter |

DMA buffers live in the kernel heap. Physical address = virtual − `KERNEL_OFFSET`. MMIO base is mapped via `mm_add_resource_map` (high physical address mapped directly).

### Descriptor rings

**TX:** polled — `send()` writes a descriptor, bumps the tail register, and polls the `DD` (descriptor done) bit to confirm completion before returning.

**RX:** interrupt-driven — the ISR fires on `E1000_ICR_RXT0` (packet received). For each completed descriptor the ISR calls `nic->rx_notify(nic->rx_ctx, buf, len)`, which is wired to the lwIP receive path.

### `nic_dev` abstraction

```c
typedef struct _nic_dev {
    uint32_t pci_dev;
    uint16_t ven, dev;
    uint8_t  mac_addr[6];
    uint8_t  ip_addr[4];
    int    (*init)(void *dev);
    void   (*on_register)(struct _nic_dev *permanent);
    int    (*send)(void *dev, const void *buf, uint16_t len);
    nic_rx_fn rx_notify;   // set by net.c: eth0_rx
    void     *rx_ctx;      // set by net.c: &eth0 (lwIP netif)
    void     *ctx;         // driver-private (e1000 state)
} nic_dev;
```

### PCI discovery (`KERNEL_INIT 6`)

`nic_scan_all` calls `pci_scan`, which enumerates all PCI devices. For Intel vendor `0x8086` with device IDs `0x100E / 0x100F / 0x1000 / 0x1001`, `nic_intel_8254x_create` allocates a `nic_dev`, initialises the hardware, and registers it in `network_devices[]`.

---

## 2. Network initialisation (`net.c`, `KERNEL_INIT 7`)

```c
void net_init(void)
{
    nic_dev *nic = nic_getdev(0);   // first registered NIC

    lwip_init();                    // initialise lwIP data structures

    // add netif with all-zero IP (DHCP will assign)
    netif_add(&eth0, 0,0,0, nic, eth0_init_fn, ethernet_input);
    netif_set_default(&eth0);
    netif_set_up(&eth0);

    // wire NIC RX → lwIP
    nic->rx_notify = eth0_rx;
    nic->rx_ctx    = &eth0;

    dhcp_start(&eth0);              // start DHCP on eth0

    // pump lwIP internal timers every 100 ms
    timer_start(lwip_timer_cb, 100, 1, NULL);
}
```

### `eth0_init_fn` — netif initialisation callback

Sets the lwIP netif fields from the `nic_dev`:

| Field | Value |
|-------|-------|
| `hwaddr` | copied from `nic->mac_addr` |
| `mtu` | 1500 |
| `flags` | `BROADCAST | ETHARP | LINK_UP` |
| `name` | `"e0"` |
| `output` | `etharp_output` (ARP → IP) |
| `linkoutput` | `eth0_linkoutput` (kernel → NIC) |

### `eth0_rx` — RX path (called from NIC ISR)

```c
static void eth0_rx(void *ctx, const uint8_t *data, uint16_t len)
{
    p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    pbuf_take(p, data, len);
    netif->input(p, netif);    // → ethernet_input → IP → TCP/UDP/ICMP
}
```

Runs in interrupt context. `netif->input` is `ethernet_input` (lwIP), which processes the frame up through the protocol stack synchronously.

### `eth0_linkoutput` — TX path

```c
static err_t eth0_linkoutput(struct netif *netif, struct pbuf *p)
{
    pbuf_copy_partial(p, buf, 1600, 0);   // flatten pbuf chain
    nic->send(nic, buf, len);             // → e1000 TX descriptor
}
```

### `lwip_timer_cb` — periodic timer pump

Called every 100 ms via the kernel timer subsystem. Calls `sys_check_timeouts()` which drives all lwIP internal timers: DHCP retransmit, ARP cache expiry, TCP retransmit / keepalive / TIME_WAIT, etc.

`sys_now()` is provided as `return (u32_t)time_now_ms()` — lwIP uses it for all timeout calculations.

---

## 3. Socket layer (`syscall_net.c`)

### `mos_sock` — per-socket object

```c
typedef struct _mos_sock {
    int domain;     // AF_INET or AF_UNIX
    int type;       // SOCK_STREAM | SOCK_DGRAM | SOCK_RAW
    int protocol;   // IPPROTO_TCP | IPPROTO_UDP | IPPROTO_ICMP
    int state;      // SS_UNCONNECTED | SS_CONNECTING | SS_CONNECTED | SS_DISCONNECTING
    int err;        // pending negative errno (0 = OK)

    union {
        struct tcp_pcb *tcp;   // AF_INET SOCK_STREAM
        struct udp_pcb *udp;   // AF_INET SOCK_DGRAM
        struct raw_pcb *raw;   // AF_INET SOCK_RAW
    };

    /* Circular receive ring (TCP/UNIX-STREAM: byte stream; UDP/RAW/UNIX-DGRAM: length-prefixed) */
    char     rxbuf[SOCK_RXBUF_SIZE];   // 8 KB
    unsigned rx_head;   // consumer index
    unsigned rx_tail;   // producer index

    struct sockaddr_in rx_src;         // UDP/RAW: source of last datagram

    struct tcp_pcb *accept_queue[SOCK_ACCEPT_BACKLOG];  // depth 8
    int accept_head, accept_tail;

    struct sockaddr_in local, peer;
    struct timeval     rx_stamp;       // timestamp of last received packet (SIOCGSTAMP)
    struct _task_struct *waiter;       // task blocked waiting for data

    struct _mos_sock  *unix_peer;      // AF_UNIX: other end of socketpair, or NULL if closed
    spinlock_t         rxbuf_lock;     // AF_UNIX: protects rx_head/rx_tail/rxbuf against
                                       // concurrent access after fork (see § AF_UNIX)
} mos_sock;
```

Each socket is exposed to user space as a VFS `file` with `i_mode = S_IFSOCK`, `i_private = mos_sock *`. The file operations vector is `sock_fops`:

```c
static const file_operations sock_fops = {
    .read    = sock_read,
    .write   = sock_write,
    .ioctl   = sock_ioctl,
    .poll    = sock_poll,
    .release = sock_release,
};
```

### Receive ring buffer

`rxbuf` is a power-of-2 circular buffer (`SOCK_RXBUF_SIZE = 8192`).

**TCP:** raw byte stream written by `tcp_on_recv`, read by `sock_read`/`do_recvfrom`.

**UDP and RAW:** each datagram is stored **length-prefixed** — a 2-byte little-endian `u16_t` length followed by the payload bytes. This preserves datagram boundaries in the ring.

```
[len_lo][len_hi][payload bytes …][len_lo][len_hi][payload bytes …]…
```

Reads first consume the 2-byte length, then the payload, discarding any bytes that don't fit in the caller's buffer.

### Blocking model

All blocking operations use `sock_wait`:

```c
static void sock_wait(mos_sock *sk, unsigned long deadline)
{
    // start one-shot kernel timer that fires at deadline
    timer_start(sock_timeout_cb, deadline - now, 0, sk);
    sk->waiter = cur;
    ps_put_to_wait_queue(cur, NULL, __func__);
    task_sched();       // yield CPU
    sk->waiter = NULL;
}
```

lwIP callbacks (`tcp_on_recv`, `udp_on_recv`, etc.) call `sock_wakeup(sk)` which calls `ps_put_to_ready_queue(sk->waiter)`. The blocked task resumes and re-checks the condition. Timeout: `SOCK_TIMEOUT_MS = 30000` ms.

`MSG_DONTWAIT` flag: checked before calling `sock_wait`; returns `-EAGAIN` immediately if no data.

---

## 4. lwIP callbacks

### TCP callbacks

| Callback | Trigger | Action |
|----------|---------|--------|
| `tcp_on_recv` | data received | copy pbuf chain → `rxbuf`; `tcp_recved(acked)`; `sock_wakeup` |
| `tcp_on_recv` (p=NULL) | remote FIN | `state = SS_DISCONNECTING`; `sock_wakeup` |
| `tcp_on_connected` | connect completed | `state = SS_CONNECTED`; `sock_wakeup` |
| `tcp_on_err` | connection reset/aborted | `tcp = NULL`; `err = -ECONNREFUSED`; `sock_wakeup` |
| `tcp_on_accept` | new connection | push `newpcb` onto `accept_queue`; `sock_wakeup` |

### UDP callback

`udp_on_recv`: stores source in `rx_src`; length-prefixes and writes datagram to `rxbuf`; `sock_wakeup`.

### RAW (ICMP) callback

`raw_on_recv`: lwIP calls this with `p->payload` pointing to the **IP header** (not stripped). The full IP + ICMP payload is stored length-prefixed in `rxbuf`. This matches Linux 2.4 `SOCK_RAW + IPPROTO_ICMP` semantics (recvmsg returns 20 IP header + 8 ICMP header + data = 84 bytes for a standard ping reply).

---

## 5. Socket operations

### `do_socket`

Supported combinations (`domain = AF_INET`):

| `type` | `protocol` | lwIP PCB |
|--------|-----------|---------|
| `SOCK_STREAM` | `IPPROTO_TCP` | `tcp_new()` |
| `SOCK_DGRAM` | `IPPROTO_UDP` | `udp_new()` |
| `SOCK_RAW` | `IPPROTO_ICMP` | `raw_new(IP_PROTO_ICMP)` |

Returns `-EAFNOSUPPORT` for any domain other than `AF_INET`. Wraps `mos_sock` in a VFS `file`, installs as fd, returns fd number.

### `do_socketpair`

Creates a pair of connected `AF_UNIX` sockets and returns two file descriptors:

```c
int do_socketpair(int domain, int type, int protocol, int sv[2]);
```

Constraints:
- `domain` must be `AF_UNIX`; other values return `-EAFNOSUPPORT`
- `type` must be `SOCK_STREAM` or `SOCK_DGRAM`; other values return `-EPROTONOSUPPORT`
- `protocol` must be `0`

Two `mos_sock` objects `a` and `b` are allocated and cross-linked (`a->unix_peer = b`, `b->unix_peer = a`). Both start in `SS_CONNECTED` state. No lwIP PCB is created. `rxbuf_lock` is initialised on both ends.

Data flow:
- `write(sv[0])` → `sock_unix_write(a)` → locked `rx_write` into `b->rxbuf` → `sock_wakeup(b)`
- `read(sv[1])` → `sock_unix_read(b)` → locked `rx_read` from `b->rxbuf`

Closing one end sets `unix_peer->unix_peer = NULL`, transitions the peer to `SS_DISCONNECTING`, and wakes any blocked reader (which then returns 0 / EOF).

### `do_bind`

Calls `tcp_bind` / `udp_bind` / `raw_bind` on the lwIP PCB. Stores `local` address.

### `do_connect`

- **RAW / UDP**: `raw_connect` / `udp_connect` — instantaneous; sets `state = SS_CONNECTED`.
- **TCP**: calls `tcp_connect` with `tcp_on_connected` callback, then blocks in `sock_wait` loop until `state != SS_CONNECTING` or timeout.

### `do_listen`

TCP only. Calls `tcp_listen_with_backlog`; registers `tcp_on_accept`; sets `state = SS_CONNECTED` (listening).

### `do_accept`

Blocks until `accept_queue` is non-empty. Dequeues the `tcp_pcb`, allocates a new `mos_sock`, calls `tcp_setup_callbacks`, wraps in a new fd.

### `do_send` / `do_sendto`

- **TCP**: `tcp_write` + `tcp_output`.
- **UDP**: `pbuf_alloc(PBUF_TRANSPORT)` + `udp_send` / `udp_sendto`.
- **RAW**: `pbuf_alloc(PBUF_IP)` + `raw_sendto`.

### `do_recv` / `do_recvfrom`

Blocks until `rxbuf` has data (or `MSG_DONTWAIT` → `-EAGAIN`). For UDP/RAW, reads the length prefix then the payload. For TCP, reads directly from the ring. Fills `from` address for UDP/RAW from `rx_src`.

### `do_shutdown`

- **TCP**: `tcp_shutdown(pcb, shut_rx, shut_tx)`.
- **RAW**: `raw_disconnect`.

---

## 6. AF_UNIX socketpair

### Why no lock is needed for AF_INET sockets

`rx_write` for TCP/UDP/raw is called exclusively from the NIC IRQ handler (`e1000_irq_handler` → `rx_notify` → lwIP → protocol callback). This is a strict **SPSC** discipline: one ISR producer writes to `rxbuf`, one task consumer reads from it. On a uniprocessor, the ISR can preempt the task but a task can never preempt an ISR, so `rx_head` and `rx_tail` are never modified concurrently — no lock required.

### Why AF_UNIX needs a lock

After `fork()`, both the parent and child hold the same fd, which points to the same `mos_sock *`. Both can call `sock_write(sv[0], ...)` from syscall context, both calling `rx_write` on `peer->rxbuf`. On a uniprocessor with preemptive scheduling a timer interrupt between any two iterations of the byte-copy loop can switch to the other task, which also enters `rx_write` — both increment `rx_tail` over the same region, corrupting the buffer and interleaving bytes.

`rxbuf_lock` is a `spinlock_t` that disables interrupts while held, preventing preemption for the duration of every ring-buffer access on the AF_UNIX path.

### Read path with lock (`sock_unix_read`)

The lock cannot be held across `sock_wait()` (which sleeps), so the pattern is: acquire, check condition, release before sleeping, re-acquire on wake, re-check:

```
spinlock_lock(&sk->rxbuf_lock)
while rx_used == 0:
    spinlock_unlock(...)
    check err / state / timeout
    sock_wait(...)        ← sleeps here, lock not held
    spinlock_lock(...)
rx_read(...)              ← under lock
spinlock_unlock(...)
```

### Write path (`sock_unix_write`)

No sleeping required. The lock wraps the entire write into the peer's buffer:

```
spinlock_lock(&peer->rxbuf_lock)
[SOCK_DGRAM: write 2-byte length prefix]
rx_write(peer, buf, n)
spinlock_unlock(...)
sock_wakeup(peer)         ← outside lock (safe)
```

### SOCK_STREAM vs SOCK_DGRAM

| | SOCK_STREAM | SOCK_DGRAM |
|-|-------------|------------|
| Ring buffer encoding | raw bytes | 2-byte length prefix + payload (same as UDP) |
| EOF on peer close | `state → SS_DISCONNECTING`, read returns 0 | timeout (`-ETIMEDOUT`) |
| `sock_poll` write-ready | `unix_peer != NULL` | `unix_peer != NULL` |

---

## 8. Socket options

**Source:** `src/net/sock_opt.c`

Both `setsockopt` and `getsockopt` are dispatched through `do_setsockopt` / `do_getsockopt`. Unknown options log a warning and return `-ENOPROTOOPT`. Unrecognised `level` values also return `-ENOPROTOOPT`.

### `SOL_SOCKET`

| Option | set | get | Notes |
|--------|-----|-----|-------|
| `SO_REUSEADDR` | ✓ | ✓ | `ip_set/reset_option(SOF_REUSEADDR)` on TCP or UDP PCB |
| `SO_KEEPALIVE` | ✓ | ✓ | `ip_set/reset_option(SOF_KEEPALIVE)` on TCP PCB; `-ENOPROTOOPT` for non-STREAM |
| `SO_ERROR` | — | ✓ | Returns `sk->err` (positive errno) and clears it to 0; `set` returns `-ENOPROTOOPT` |
| `SO_TYPE` | — | ✓ | Returns `sk->type` (`SOCK_STREAM` / `SOCK_DGRAM` / `SOCK_RAW`) |
| `SO_RCVBUF` | ignored | ✓ | `get` always returns `SOCK_RXBUF_SIZE` (8192); `set` is silently accepted |
| `SO_SNDBUF` | ignored | ✓ | `get` returns `tcp_sndbuf(sk->tcp)` for TCP, 0 otherwise; `set` is silently accepted |
| `SO_LINGER` | ignored | — | silently accepted |
| `SO_RCVTIMEO` | ignored | — | silently accepted |
| `SO_SNDTIMEO` | ignored | — | silently accepted |
| `SO_TIMESTAMP` | ✓ | ✓ | Enables/disables `SOCK_CMSG_TIMESTAMP` flag; causes `recvmsg` to attach a `struct timeval` cmsg (`SOL_SOCKET / SCM_TIMESTAMP`) to each received message |

### `IPPROTO_IP` / `IPPROTO_ICMP` / `IPPROTO_RAW`

All three level values are handled by the same branch.

| Option | set | get | Notes |
|--------|-----|-----|-------|
| `IP_HDRINCL` | ✓ | ✓ | `SOCK_RAW` only; application supplies the full IP header on `send`; `-ENOPROTOOPT` for other types |
| `IP_TTL` | ✓ | ✓ | Enables/disables `SOCK_CMSG_TTL` flag; causes `recvmsg` to attach an `int` TTL cmsg (`IPPROTO_IP / IP_TTL`) |
| `IP_PKTINFO` | ✓ | ✓ | Enables/disables `SOCK_CMSG_PKTINFO` flag; causes `recvmsg` to attach a `struct in_pktinfo` cmsg (`IPPROTO_IP / IP_PKTINFO`) with destination address and interface index |
| `ICMP_FILTER` | ✓ | ✓ | `SOCK_RAW + IPPROTO_ICMP` only; `set` stores a `struct icmp_filter` bitmask in `sk->icmp_filter` (bit N set → drop ICMP type N); `get` returns current bitmask |
| `IP_RECVERR` | ignored | — | silently accepted |

### `IPPROTO_TCP`

Only valid on `SOCK_STREAM` sockets; other types return `-ENOPROTOOPT`.

| Option | set | get | Notes |
|--------|-----|-----|-------|
| `TCP_NODELAY` | ✓ | ✓ | `tcp_nagle_disable` / `tcp_nagle_enable` on lwIP PCB |
| `TCP_KEEPIDLE` | ignored | — | silently accepted |
| `TCP_MAXSEG` | — | ✓ | Returns `sk->tcp->mss`; falls back to 536 if PCB is not yet connected |

### Ancillary data (cmsg) produced by `recvmsg`

When enabled via `setsockopt`, the following control messages are appended to the `msg_control` buffer on each `recvmsg` call:

| Flag | `cmsg_level` | `cmsg_type` | Data type | Source |
|------|-------------|-------------|-----------|--------|
| `SOCK_CMSG_TIMESTAMP` | `SOL_SOCKET` | `SO_TIMESTAMP` | `struct timeval` | `sk->rx_stamp` (set in `rx_write`) |
| `SOCK_CMSG_TTL` | `IPPROTO_IP` | `IP_TTL` | `int` | `sk->rx_ttl` (set in RAW/UDP callbacks) |
| `SOCK_CMSG_PKTINFO` | `IPPROTO_IP` | `IP_PKTINFO` | `struct in_pktinfo` | `sk->rx_dst` / `sk->rx_ifindex` (set in callbacks) |

---

## 7. ioctl

`sock_ioctl` handles:

| `cmd` | Behaviour |
|-------|-----------|
| `SIOCGSTAMP` | copy `sk->rx_stamp` to user — timestamp of last received packet |
| `FIONREAD` | return `rx_used(sk)` — bytes available |
| `FIONBIO` | silently accepted |
| `SIOCGIFCONF` | fill `ifconf` with `eth0` and `lo` entries |
| `SIOCGIFFLAGS` | `IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST` for eth0; `IFF_LOOPBACK` for lo |
| `SIOCGIFADDR` | interface IPv4 address |
| `SIOCGIFNETMASK` | network mask |
| `SIOCGIFBRDADDR` | broadcast address (`ip | ~mask`) |
| `SIOCGIFHWADDR` | MAC address (`sa_family = ARPHRD_ETHER`) |
| `SIOCGIFMTU` | MTU (1500 for eth0, 65536 for lo) |
| `SIOCGIFINDEX` | interface index (dynamic: `netif->num + 1`; lo = 1, eth0 = 2) |

Interface names are lwIP-derived: `"e00"` for eth0 (matches `nif->name[0..1] + num`), `"lo"` for loopback.

The loopback interface (`lo`) is a real lwIP `netif` (127.0.0.1/8, name `"lo0"`) created automatically by `lwip_init()` when `LWIP_NETIF_LOOPBACK=1`. Because it is the first netif added, it gets `num=0` (index 1); eth0 gets `num=1` (index 2). In NO_SYS mode, loopback packets are delivered synchronously during the 100 ms periodic timer via `netif_poll_all()`.

---

## 9. `sys_socketcall` dispatch

User space invokes socket operations via a single `sys_socketcall` (Linux i386 syscall 102). The first argument is the sub-call number:

| Sub-call | Number | Function |
|----------|--------|----------|
| `SYS_SOCKET` | 1 | `do_socket` |
| `SYS_BIND` | 2 | `do_bind` |
| `SYS_CONNECT` | 3 | `do_connect` |
| `SYS_LISTEN` | 4 | `do_listen` |
| `SYS_ACCEPT` | 5 | `do_accept` |
| `SYS_GETSOCKNAME` | 6 | `do_getsockname` |
| `SYS_GETPEERNAME` | 7 | `do_getpeername` |
| `SYS_SOCKETPAIR` | 8 | `do_socketpair` |
| `SYS_SEND` | 9 | `do_send` |
| `SYS_RECV` | 10 | `do_recv` |
| `SYS_SENDTO` | 11 | `do_sendto` |
| `SYS_RECVFROM` | 12 | `do_recvfrom` |
| `SYS_SHUTDOWN` | 13 | `do_shutdown` |
| `SYS_SETSOCKOPT` | 14 | `do_setsockopt` |
| `SYS_GETSOCKOPT` | 15 | `do_getsockopt` |
| `SYS_SENDMSG` | 16 | `do_sendmsg` |
| `SYS_RECVMSG` | 17 | `do_recvmsg` |
| `SYS_ACCEPT4` | 18 | `do_accept` (flags ignored) |

---

## 10. Lifecycle summary

```
KERNEL_INIT 6: nic_scan_all
  └─ pci_scan → 8086:100E → nic_intel_8254x_create
       └─ MMIO map, reset chip, read MAC, init RX/TX descriptor rings
       └─ register IRQ handler (interrupt-driven RX)
       └─ nic_register → network_devices[0]

KERNEL_INIT 7: net_init
  └─ lwip_init()
  └─ netif_add(eth0, ..., eth0_init_fn, ethernet_input)
  └─ nic->rx_notify = eth0_rx; nic->rx_ctx = &eth0
  └─ dhcp_start(eth0)
  └─ timer_start(lwip_timer_cb, 100ms, repeating)

NIC receives Ethernet frame
  └─ e1000 IRQ → ISR → nic->rx_notify(ctx, buf, len)
  └─ eth0_rx: pbuf_alloc + pbuf_take + netif->input
  └─ ethernet_input → IP → TCP/UDP/ICMP dispatch
  └─ lwIP callback (tcp_on_recv / udp_on_recv / raw_on_recv)
       └─ rx_write → rxbuf ring
       └─ sock_wakeup → ps_put_to_ready_queue(waiter)

socket(AF_INET, SOCK_STREAM, 0)
  └─ do_socket → tcp_new, tcp_setup_callbacks, sock_to_fd → fd

connect(fd, addr)
  └─ do_connect → tcp_connect → SS_CONNECTING
  └─ sock_wait → task blocks
  └─ tcp_on_connected fires (from NIC ISR path) → SS_CONNECTED, sock_wakeup
  └─ task resumes, returns 0

send(fd, buf, len)
  └─ do_send → sock_write → tcp_write + tcp_output
  └─ lwIP → eth0_linkoutput → nic->send → e1000 TX

recv(fd, buf, len)
  └─ do_recv → do_recvfrom → sock_wait (if no data)
  └─ tcp_on_recv wakes task → rx_read from rxbuf → return n

close(fd)
  └─ fs_close → fs_put_file → sock_release
  └─ AF_INET: tcp_close / udp_remove / raw_remove
  └─ AF_UNIX: peer->unix_peer = NULL, peer->state = SS_DISCONNECTING, sock_wakeup(peer)
  └─ free(sk)

socketpair(AF_UNIX, SOCK_STREAM, 0, sv)
  └─ do_socketpair → alloc mos_sock a, b; cross-link unix_peer; spinlock_init
  └─ sock_to_fd(a) → sv[0], sock_to_fd(b) → sv[1]

write(sv[0], buf, len)   [parent or child after fork]
  └─ sock_unix_write(a) → spinlock_lock(b->rxbuf_lock)
  └─ rx_write(b, buf, len) → spinlock_unlock → sock_wakeup(b)

read(sv[1], buf, len)
  └─ sock_unix_read(b) → spinlock_lock(b->rxbuf_lock)
  └─ [if empty] spinlock_unlock → sock_wait → spinlock_lock [on wake]
  └─ rx_read(b, buf, len) → spinlock_unlock → return n
```
