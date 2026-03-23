# Network Stack

**Source:** `src/hw/nic_intel_8254x.c`, `src/hw/nic.c`, `src/net/net.c`, `src/syscall/syscall_net.c`
**Headers:** `include/hw/nic.h`, `include/net/net.h`, `include/net/socket.h`

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
    int domain;     // AF_INET
    int type;       // SOCK_STREAM | SOCK_DGRAM | SOCK_RAW
    int protocol;   // IPPROTO_TCP | IPPROTO_UDP | IPPROTO_ICMP
    int state;      // SS_UNCONNECTED | SS_CONNECTING | SS_CONNECTED | SS_DISCONNECTING
    int err;        // pending negative errno (0 = OK)

    union {
        struct tcp_pcb *tcp;
        struct udp_pcb *udp;
        struct raw_pcb *raw;
    };

    /* Circular receive ring (TCP: byte stream; UDP/RAW: length-prefixed datagrams) */
    char     rxbuf[SOCK_RXBUF_SIZE];   // 8 KB
    unsigned rx_head;   // consumer index
    unsigned rx_tail;   // producer index

    struct sockaddr_in rx_src;         // UDP/RAW: source of last datagram

    struct tcp_pcb *accept_queue[SOCK_ACCEPT_BACKLOG];  // depth 8
    int accept_head, accept_tail;

    struct sockaddr_in local, peer;
    struct timeval     rx_stamp;       // timestamp of last received packet (SIOCGSTAMP)
    struct _task_struct *waiter;       // task blocked waiting for data
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

Supported combinations:

| `type` | `protocol` | lwIP PCB |
|--------|-----------|---------|
| `SOCK_STREAM` | `IPPROTO_TCP` | `tcp_new()` |
| `SOCK_DGRAM` | `IPPROTO_UDP` | `udp_new()` |
| `SOCK_RAW` | `IPPROTO_ICMP` | `raw_new(IP_PROTO_ICMP)` |

Wraps `mos_sock` in a VFS `file`, installs as fd, returns fd number.

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

## 6. Socket options

### `SOL_SOCKET`

| Option | Behaviour |
|--------|-----------|
| `SO_REUSEADDR` | `ip_set_option(SOF_REUSEADDR)` on TCP/UDP PCB |
| `SO_KEEPALIVE` | `ip_set_option(SOF_KEEPALIVE)` on TCP PCB |
| `SO_RCVBUF` / `SO_SNDBUF` | silently accepted (fixed 8 KB ring) |
| `SO_LINGER` | silently accepted |
| `SO_ERROR` | returns and clears `sk->err` |
| `SO_TYPE` | returns `sk->type` |

### `IPPROTO_TCP`

| Option | Behaviour |
|--------|-----------|
| `TCP_NODELAY` | `tcp_nagle_disable` / `tcp_nagle_enable` |
| `TCP_KEEPIDLE` | silently accepted |

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
| `SIOCGIFINDEX` | interface index (1 = eth0, 2 = lo) |

Interface names are lwIP-derived: `"e00"` for eth0 (matches `nif->name[0..1] + num`), `"lo"` for loopback.

---

## 8. `sys_socketcall` dispatch

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

## 9. Lifecycle summary

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
  └─ tcp_close / udp_remove / raw_remove
  └─ free(sk)
```
