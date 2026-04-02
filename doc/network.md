# Network Stack

**Source:** `src/hw/nic_intel_8254x.c`, `src/hw/nic.c`, `src/net/net.c`, `src/net/sock.c`, `src/net/sock_ops.c`, `src/net/sock_cb.c`, `src/net/sock_msg.c`, `src/net/sock_opt.c`, `src/net/sock_un.c`, `src/syscall/syscall_net.c`
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

lwIP runs in **NO_SYS mode** — no internal threads. All protocol processing happens synchronously inside DSR callbacks (deferred from the NIC IRQ) or explicit `sys_check_timeouts()` calls.

---

## 1. NIC driver — Intel 8254x (e1000)

**File:** `src/hw/nic_intel_8254x.c`
**Supported hardware:** Intel 82540EM (PCI `8086:100E`) — the QEMU default `e1000`.

### Hardware interface

Accessed via MMIO at BAR0. Key register groups:

| Register              | Offset       | Purpose                                            |
| --------------------- | ------------ | -------------------------------------------------- |
| `CTRL`                | 0x0000       | Control (reset, link-up)                           |
| `ICR` / `IMS` / `IMC` | 0x00C0/D0/D8 | Interrupt cause / mask set / mask clear            |
| `RCTL`                | 0x0100       | Receive control (enable, buffer size, promiscuous) |
| `TCTL` / `TIPG`       | 0x0400/10    | Transmit control / inter-packet gap                |
| `RDBAL/H/LEN/H/T`     | 0x2800…      | RX descriptor ring base, length, head, tail        |
| `TDBAL/H/LEN/H/T`     | 0x3800…      | TX descriptor ring base, length, head, tail        |
| `RAL0` / `RAH0`       | 0x5400/04    | Receive address (MAC) filter                       |

DMA buffers live in the kernel heap. Physical address = virtual − `KERNEL_OFFSET`. MMIO base is mapped via `mm_map_io` (high physical address mapped directly).

### Descriptor rings

**TX:** polled — `send()` writes a descriptor, bumps the tail register, and polls the `DD` (descriptor done) bit to confirm completion before returning.

**RX:** interrupt-driven — the ISR fires on `E1000_ICR_RXT0` (packet received). For each completed descriptor the ISR calls `nic->rx_notify(nic->rx_ctx, buf, len)`, which enqueues the raw frame into a packet ring (no malloc). A DSR then drains the ring and feeds frames into lwIP outside of IRQ context.

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
    nic_rx_fn rx_notify;   // set by net.c: eth0_rx_enqueue (IRQ-safe, no malloc)
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

    // wire NIC RX → packet ring enqueue
    nic->rx_notify = eth0_rx_enqueue;
    nic->rx_ctx    = &eth0;

    dhcp_start(&eth0);              // start DHCP on eth0

    // pump lwIP internal timers every 100 ms
    timer_start(lwip_timer_cb, 100, 1, NULL);
}
```

### `eth0_init_fn` — netif initialisation callback

Sets the lwIP netif fields from the `nic_dev`:

| Field        | Value                            |
| ------------ | -------------------------------- |
| `hwaddr`     | copied from `nic->mac_addr`      |
| `mtu`        | 1500                             |
| `flags`      | `BROADCAST                       | ETHARP | LINK_UP` |
| `name`       | `"e0"`                           |
| `output`     | `etharp_output` (ARP → IP)       |
| `linkoutput` | `eth0_linkoutput` (kernel → NIC) |

### RX path — two-phase deferred design

The NIC ISR must not call `malloc` or lwIP directly: it may preempt a process that is itself inside `malloc`, causing a deadlock on the heap lock. The receive path is therefore split into two phases.

**Phase 1 — `eth0_rx_enqueue` (runs in NIC IRQ context, no malloc):**

```c
static void eth0_rx_enqueue(void *ctx, const uint8_t *data, uint16_t len)
{
    // drop if ring full
    if (g_rx_wr - g_rx_rd >= NET_RX_RING_SIZE) return;
    slot = &g_rx_ring[g_rx_wr % NET_RX_RING_SIZE];
    memcpy(slot->data, data, len);   // only a memcpy — no malloc
    slot->len = len;
    g_rx_wr++;
    if (!g_rx_dsr_armed) {
        g_rx_dsr_armed = 1;
        dsr_add(eth0_rx_dsr, NULL);  // schedule phase 2
    }
}
```

Ring capacity: `NET_RX_RING_SIZE = 32` slots × `NET_RX_MAX_FRAME = 1600` bytes = 51 KB (BSS).

**Phase 2 — `eth0_rx_dsr` (runs as a DSR from `_task_sched`, malloc is safe):**

```c
static void eth0_rx_dsr(void *param)
{
    g_rx_dsr_armed = 0;   // clear first so concurrent IRQs can re-arm
    while (g_rx_rd != g_rx_wr) {
        slot = &g_rx_ring[g_rx_rd % NET_RX_RING_SIZE];
        p = pbuf_alloc(PBUF_RAW, slot->len, PBUF_POOL);   // malloc safe here
        pbuf_take(p, slot->data, slot->len);
        eth0.input(p, &eth0);   // → ethernet_input → IP → TCP/UDP/ICMP
        g_rx_rd++;
    }
}
```

DSRs are drained by `dsr_drain()` at the top of `_task_sched()` (before every context switch), so frames are processed promptly without polling.

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
    .poll_wait = sock_poll_wait,
	  .poll_wait_remove = sock_poll_wait_remove,
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

lwIP callbacks (`tcp_on_recv`, `udp_on_recv`, etc.) call `sock_wakeup(sk)` which calls `ps_put_to_ready_queue(sk->waiter)`. These callbacks run from `eth0_rx_dsr` (DSR context, interrupts disabled), so `ps_put_to_ready_queue` marks the task runnable but the actual context switch happens when `_task_sched` selects the next task after `dsr_drain` returns. The blocked task resumes and re-checks the condition. Timeout: `SOCK_TIMEOUT_MS = 30000` ms.

`MSG_DONTWAIT` flag: checked before calling `sock_wait`; returns `-EAGAIN` immediately if no data.

---

## 4. lwIP callbacks

### TCP callbacks

| Callback               | Trigger                  | Action                                                        |
| ---------------------- | ------------------------ | ------------------------------------------------------------- |
| `tcp_on_recv`          | data received            | copy pbuf chain → `rxbuf`; `tcp_recved(acked)`; `sock_wakeup` |
| `tcp_on_recv` (p=NULL) | remote FIN               | `state = SS_DISCONNECTING`; `sock_wakeup`                     |
| `tcp_on_connected`     | connect completed        | `state = SS_CONNECTED`; `sock_wakeup`                         |
| `tcp_on_err`           | connection reset/aborted | `tcp = NULL`; `err = -ECONNREFUSED`; `sock_wakeup`            |
| `tcp_on_accept`        | new connection           | push `newpcb` onto `accept_queue`; `sock_wakeup`              |

### UDP callback

`udp_on_recv`: stores source in `rx_src`; length-prefixes and writes datagram to `rxbuf`; `sock_wakeup`.

### RAW (ICMP) callback

`raw_on_recv`: lwIP calls this with `p->payload` pointing to the **IP header** (not stripped). The full IP + ICMP payload is stored length-prefixed in `rxbuf`. This matches Linux 2.4 `SOCK_RAW + IPPROTO_ICMP` semantics (recvmsg returns 20 IP header + 8 ICMP header + data = 84 bytes for a standard ping reply).

---

## 5. Socket operations

### `do_socket`

Supported combinations (`domain = AF_INET`):

| `type`        | `protocol`     | lwIP PCB                 |
| ------------- | -------------- | ------------------------ |
| `SOCK_STREAM` | `IPPROTO_TCP`  | `tcp_new()`              |
| `SOCK_DGRAM`  | `IPPROTO_UDP`  | `udp_new()`              |
| `SOCK_RAW`    | `IPPROTO_ICMP` | `raw_new(IP_PROTO_ICMP)` |

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

## 6. AF_UNIX domain sockets

**Source:** `src/net/sock_un.c`, `src/net/sock.c`, `src/net/sock_ops.c`

AF_UNIX sockets bypass the NIC and lwIP entirely. Data moves directly through an in-kernel ring buffer from writer to reader via the `unix_peer` pointer.

### Relevant `mos_sock` fields

```c
char      unix_path[UNIX_PATH_MAX];          // bound filesystem path (or "" if unbound)
mos_sock *unix_peer;                         // pointer to the other side's mos_sock
mos_sock *unix_accept_queue[SOCK_ACCEPT_BACKLOG]; // pending server sockets (listener)
int       unix_accept_head, unix_accept_tail;
spinlock_t rxbuf_lock;                       // protects rxbuf on the AF_UNIX path
```

`UNIX_PATH_MAX = 108`, `SOCK_ACCEPT_BACKLOG = 8`.

### Unix socket namespace

A flat array of 64 `{path, mos_sock*}` entries, protected by `unix_ns_lock` (mutex). The namespace maps resolved filesystem paths to the `mos_sock` that bound them.

```c
#define UNIX_NS_MAX 64
typedef struct { char path[UNIX_PATH_MAX]; mos_sock *sk; } unix_ns_entry;
static unix_ns_entry unix_ns[UNIX_NS_MAX];
static mutex_t       unix_ns_lock;
```

Paths are resolved via `resolve_path` (cwd-relative → absolute) before lookup/insert, so the same socket is always found regardless of how the caller spelled the path.

Unlink integration: `sys_unlink` calls `unix_ns_remove_path(path)` when a socket file is unlinked while the socket is still open. This clears `unix_ns[i].sk->unix_path` so `unix_release` will not attempt a second `vfs_umount` on the already-removed file.

### Named socket lifecycle

**Server side:**

```
socket(AF_UNIX, SOCK_STREAM, 0)
  └─ do_socket: alloc mos_sock, domain=AF_UNIX, state=SS_UNCONNECTED

bind(fd, {sun_family=AF_UNIX, sun_path="/tmp/foo"}, sizeof)
  └─ unix_bind:
       resolve_path("/tmp/foo") → abs_path
       mutex_lock(unix_ns_lock)
         unix_ns_lookup(abs_path) → must not exist (else EADDRINUSE)
         vfs_mknod(cur->root, abs_path, S_IFSOCK|0777, 0)   // create socket file
         strncpy(sk->unix_path, abs_path)
         unix_ns_register(abs_path, sk)
       mutex_unlock

listen(fd, backlog)
  └─ unix_listen: checks unix_path set; sk->state = SS_CONNECTED (reuses listening state)

accept(fd, addr, addrlen)            // blocks until connect() enqueues a server_sk
  └─ unix_accept:
       while unix_accept_head == unix_accept_tail:
           sock_wait(listener, deadline)  // sleep until woken by unix_connect
       server_sk = unix_accept_queue[head++]
       if addr: fill sockaddr_un with client's unix_path
       sock_to_fd(server_sk) → new fd
```

**Client side:**

```
socket(AF_UNIX, SOCK_STREAM, 0)
  └─ do_socket: alloc mos_sock, domain=AF_UNIX, state=SS_UNCONNECTED

connect(fd, {sun_family=AF_UNIX, sun_path="/tmp/foo"}, sizeof)
  └─ unix_connect:
       resolve_path("/tmp/foo") → abs_path
       mutex_lock(unix_ns_lock)
         listener = unix_ns_lookup(abs_path)
         check: listener exists, state==SS_CONNECTED, type==SOCK_STREAM
         check: accept queue not full (next_tail != head → else ECONNREFUSED)

         server_sk = zalloc(mos_sock)   // pre-create the server side
         server_sk->domain  = AF_UNIX
         server_sk->type    = client->type
         server_sk->state   = SS_CONNECTED
         server_sk->unix_peer = client
         strncpy(server_sk->unix_path, abs_path)  // copy listener path
                                                   // NOT registered in unix_ns

         client->unix_peer = server_sk
         client->state     = SS_CONNECTED

         listener->unix_accept_queue[tail] = server_sk
         listener->unix_accept_tail = next_tail
       mutex_unlock
       sock_wakeup(listener)            // wake accept()
```

`unix_connect` is **synchronous from the caller's perspective**: it never blocks. The server-side `mos_sock` is fully constructed and cross-linked inside the mutex, then enqueued. The accept-side task is woken up. No handshake occurs.

### `socketpair` (unnamed)

```
socketpair(AF_UNIX, SOCK_STREAM|SOCK_DGRAM, 0, sv)
  └─ do_socketpair:
       alloc mos_sock a, b
       a->unix_peer = b;  b->unix_peer = a   // cross-link immediately
       a->state = b->state = SS_CONNECTED
       spinlock_init(&a->rxbuf_lock)
       spinlock_init(&b->rxbuf_lock)
       sv[0] = sock_to_fd(a)
       sv[1] = sock_to_fd(b)
```

Neither socket has a `unix_path` or namespace entry. Works for both `SOCK_STREAM` and `SOCK_DGRAM`.

### Data flow (read / write)

All data travels through the **receiver's** `rxbuf` ring (8 KB, `SOCK_RXBUF_SIZE`).

**Write** (`sock_unix_write`):

```
peer = sk->unix_peer
if peer == NULL: return -EPIPE

spinlock_lock(&peer->rxbuf_lock)         // IRQs disabled — prevent preemption
if SOCK_DGRAM:
    rx_write(peer, &dlen, 2)             // 2-byte length prefix
rx_write(peer, buf, count)               // copy into peer's rxbuf
spinlock_unlock(...)
sock_wakeup(peer)                        // wake peer's read() if blocked
```

Write is non-blocking — returns `-ENOBUFS` if the peer ring is full (DGRAM) or writes as many bytes as fit (STREAM). No sleep.

**Read** (`sock_unix_read`):

```
SOCK_STREAM:
  spinlock_lock(&sk->rxbuf_lock)
  while rx_used == 0:
      spinlock_unlock(...)
      if state == SS_DISCONNECTING: return 0    // EOF
      sock_wait(sk, deadline)                   // sleep here, lock not held
      spinlock_lock(...)
  rx_read(sk, buf, count)
  spinlock_unlock(...)
  return n

SOCK_DGRAM:
  spinlock_lock(&sk->rxbuf_lock)
  while rx_used < 2:                            // wait for at least a length prefix
      spinlock_unlock(...)
      sock_wait(sk, deadline)
      spinlock_lock(...)
  rx_read(sk, &dlen, 2)                         // read 2-byte length
  n = min(count, dlen)
  rx_read(sk, buf, n)
  discard remaining (dlen - n) bytes if buf too small
  spinlock_unlock(...)
  return n
```

### Peer close / shutdown

```
close(fd)
  └─ unix_release(sk):
       peer = sk->unix_peer
       if peer:
           peer->unix_peer = NULL
           peer->state = SS_DISCONNECTING
           sock_wakeup(peer)            // unblocks peer's read → returns 0

       if sk->unix_path set AND registered in unix_ns:
           unix_ns_unregister(sk)
           vfs_umount(cur->root, sk->unix_path)  // remove socket file

       drain accept_queue:              // for listener: free pending server_sks
           for each pending server_sk:
               pending->unix_peer->state = SS_DISCONNECTING
               sock_wakeup(pending->unix_peer)
               free(pending)

shutdown(fd, how)
  └─ same as close for AF_UNIX: disconnects peer immediately, ignores how
```

### Why AF_UNIX needs a spinlock (but AF_INET does not)

For AF_INET, `rx_write` is called exclusively from `eth0_rx_dsr` (DSR context, interrupts disabled) — a strict SPSC relationship with the reader task. No concurrent producers are possible.

For AF_UNIX, after `fork()` both the parent and child hold the same fd pointing to the same `mos_sock *`. Both can call `sock_unix_write` concurrently from syscall context (preemptive, different tasks). Without a lock, a timer interrupt between iterations of the byte-copy loop can switch tasks, causing two concurrent `rx_write` calls to both advance `rx_tail` over the same region — corrupting the ring and interleaving bytes.

`rxbuf_lock` is a spinlock (disables IRQs while held) so a single lock simultaneously prevents both task preemption and DSR re-entry for the duration of each ring-buffer access.

### SOCK_STREAM vs SOCK_DGRAM comparison

|                      | SOCK_STREAM                                | SOCK_DGRAM                                          |
| -------------------- | ------------------------------------------ | --------------------------------------------------- |
| Ring encoding        | raw byte stream                            | 2-byte length prefix + payload per message          |
| Message boundaries   | none (byte stream)                         | preserved (length-prefixed framing)                 |
| EOF on peer close    | `state → SS_DISCONNECTING`, read returns 0 | peer close not signalled; read blocks until timeout |
| connect required     | yes (`unix_connect` cross-links peers)     | no (but only `socketpair` is usable without bind)   |
| Write if peer closed | `-EPIPE`                                   | `-EPIPE`                                            |
| poll write-ready     | `unix_peer != NULL`                        | `unix_peer != NULL`                                 |

---

## 8. Socket options

**Source:** `src/net/sock_opt.c`

Both `setsockopt` and `getsockopt` are dispatched through `do_setsockopt` / `do_getsockopt`. Unknown options log a warning and return `-ENOPROTOOPT`. Unrecognised `level` values also return `-ENOPROTOOPT`.

### `SOL_SOCKET`

| Option         | set     | get | Notes                                                                                                                                                   |
| -------------- | ------- | --- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `SO_REUSEADDR` | ✓       | ✓   | `ip_set/reset_option(SOF_REUSEADDR)` on TCP or UDP PCB                                                                                                  |
| `SO_KEEPALIVE` | ✓       | ✓   | `ip_set/reset_option(SOF_KEEPALIVE)` on TCP PCB; `-ENOPROTOOPT` for non-STREAM                                                                          |
| `SO_ERROR`     | —       | ✓   | Returns `sk->err` (positive errno) and clears it to 0; `set` returns `-ENOPROTOOPT`                                                                     |
| `SO_TYPE`      | —       | ✓   | Returns `sk->type` (`SOCK_STREAM` / `SOCK_DGRAM` / `SOCK_RAW`)                                                                                          |
| `SO_RCVBUF`    | ignored | ✓   | `get` always returns `SOCK_RXBUF_SIZE` (8192); `set` is silently accepted                                                                               |
| `SO_SNDBUF`    | ignored | ✓   | `get` returns `tcp_sndbuf(sk->tcp)` for TCP, 0 otherwise; `set` is silently accepted                                                                    |
| `SO_LINGER`    | ignored | —   | silently accepted                                                                                                                                       |
| `SO_RCVTIMEO`  | ignored | —   | silently accepted                                                                                                                                       |
| `SO_SNDTIMEO`  | ignored | —   | silently accepted                                                                                                                                       |
| `SO_TIMESTAMP` | ✓       | ✓   | Enables/disables `SOCK_CMSG_TIMESTAMP` flag; causes `recvmsg` to attach a `struct timeval` cmsg (`SOL_SOCKET / SCM_TIMESTAMP`) to each received message |

### `IPPROTO_IP` / `IPPROTO_ICMP` / `IPPROTO_RAW`

All three level values are handled by the same branch.

| Option        | set     | get | Notes                                                                                                                                                                     |
| ------------- | ------- | --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `IP_HDRINCL`  | ✓       | ✓   | `SOCK_RAW` only; application supplies the full IP header on `send`; `-ENOPROTOOPT` for other types                                                                        |
| `IP_TTL`      | ✓       | ✓   | Enables/disables `SOCK_CMSG_TTL` flag; causes `recvmsg` to attach an `int` TTL cmsg (`IPPROTO_IP / IP_TTL`)                                                               |
| `IP_PKTINFO`  | ✓       | ✓   | Enables/disables `SOCK_CMSG_PKTINFO` flag; causes `recvmsg` to attach a `struct in_pktinfo` cmsg (`IPPROTO_IP / IP_PKTINFO`) with destination address and interface index |
| `ICMP_FILTER` | ✓       | ✓   | `SOCK_RAW + IPPROTO_ICMP` only; `set` stores a `struct icmp_filter` bitmask in `sk->icmp_filter` (bit N set → drop ICMP type N); `get` returns current bitmask            |
| `IP_RECVERR`  | ignored | —   | silently accepted                                                                                                                                                         |

### `IPPROTO_TCP`

Only valid on `SOCK_STREAM` sockets; other types return `-ENOPROTOOPT`.

| Option         | set     | get | Notes                                                                 |
| -------------- | ------- | --- | --------------------------------------------------------------------- |
| `TCP_NODELAY`  | ✓       | ✓   | `tcp_nagle_disable` / `tcp_nagle_enable` on lwIP PCB                  |
| `TCP_KEEPIDLE` | ignored | —   | silently accepted                                                     |
| `TCP_MAXSEG`   | —       | ✓   | Returns `sk->tcp->mss`; falls back to 536 if PCB is not yet connected |

### Ancillary data (cmsg) produced by `recvmsg`

When enabled via `setsockopt`, the following control messages are appended to the `msg_control` buffer on each `recvmsg` call:

| Flag                  | `cmsg_level` | `cmsg_type`    | Data type           | Source                                             |
| --------------------- | ------------ | -------------- | ------------------- | -------------------------------------------------- |
| `SOCK_CMSG_TIMESTAMP` | `SOL_SOCKET` | `SO_TIMESTAMP` | `struct timeval`    | `sk->rx_stamp` (set in `rx_write`)                 |
| `SOCK_CMSG_TTL`       | `IPPROTO_IP` | `IP_TTL`       | `int`               | `sk->rx_ttl` (set in RAW/UDP callbacks)            |
| `SOCK_CMSG_PKTINFO`   | `IPPROTO_IP` | `IP_PKTINFO`   | `struct in_pktinfo` | `sk->rx_dst` / `sk->rx_ifindex` (set in callbacks) |

---

## 7. ioctl

`sock_ioctl` handles:

| `cmd`            | Behaviour                                                       |
| ---------------- | --------------------------------------------------------------- |
| `SIOCGSTAMP`     | copy `sk->rx_stamp` to user — timestamp of last received packet |
| `FIONREAD`       | return `rx_used(sk)` — bytes available                          |
| `FIONBIO`        | silently accepted                                               |
| `SIOCGIFCONF`    | fill `ifconf` with `eth0` and `lo` entries                      |
| `SIOCGIFFLAGS`   | `IFF_UP                                                         | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST` for eth0; `IFF_LOOPBACK` for lo |
| `SIOCGIFADDR`    | interface IPv4 address                                          |
| `SIOCGIFNETMASK` | network mask                                                    |
| `SIOCGIFBRDADDR` | broadcast address (`ip                                          | ~mask`)     |
| `SIOCGIFHWADDR`  | MAC address (`sa_family = ARPHRD_ETHER`)                        |
| `SIOCGIFMTU`     | MTU (1500 for eth0, 65536 for lo)                               |
| `SIOCGIFINDEX`   | interface index (dynamic: `netif->num + 1`; lo = 1, eth0 = 2)   |

Interface names are lwIP-derived: `"e00"` for eth0 (matches `nif->name[0..1] + num`), `"lo"` for loopback.

The loopback interface (`lo`) is a real lwIP `netif` (127.0.0.1/8, name `"lo0"`) created automatically by `lwip_init()` when `LWIP_NETIF_LOOPBACK=1`. Because it is the first netif added, it gets `num=0` (index 1); eth0 gets `num=1` (index 2). In NO_SYS mode, loopback packets are delivered synchronously during the 100 ms periodic timer via `netif_poll_all()`.

---

## 9. `sys_socketcall` dispatch

User space invokes socket operations via a single `sys_socketcall` (Linux i386 syscall 102). The first argument is the sub-call number:

| Sub-call          | Number | Function                    |
| ----------------- | ------ | --------------------------- |
| `SYS_SOCKET`      | 1      | `do_socket`                 |
| `SYS_BIND`        | 2      | `do_bind`                   |
| `SYS_CONNECT`     | 3      | `do_connect`                |
| `SYS_LISTEN`      | 4      | `do_listen`                 |
| `SYS_ACCEPT`      | 5      | `do_accept`                 |
| `SYS_GETSOCKNAME` | 6      | `do_getsockname`            |
| `SYS_GETPEERNAME` | 7      | `do_getpeername`            |
| `SYS_SOCKETPAIR`  | 8      | `do_socketpair`             |
| `SYS_SEND`        | 9      | `do_send`                   |
| `SYS_RECV`        | 10     | `do_recv`                   |
| `SYS_SENDTO`      | 11     | `do_sendto`                 |
| `SYS_RECVFROM`    | 12     | `do_recvfrom`               |
| `SYS_SHUTDOWN`    | 13     | `do_shutdown`               |
| `SYS_SETSOCKOPT`  | 14     | `do_setsockopt`             |
| `SYS_GETSOCKOPT`  | 15     | `do_getsockopt`             |
| `SYS_SENDMSG`     | 16     | `do_sendmsg`                |
| `SYS_RECVMSG`     | 17     | `do_recvmsg`                |
| `SYS_ACCEPT4`     | 18     | `do_accept` (flags ignored) |

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
  └─ nic->rx_notify = eth0_rx_enqueue; nic->rx_ctx = &eth0
  └─ dhcp_start(eth0)
  └─ timer_start(lwip_timer_cb, 100ms, repeating)

NIC receives Ethernet frame
  └─ e1000 IRQ → ISR → nic->rx_notify(ctx, buf, len)
  └─ eth0_rx_enqueue: memcpy into g_rx_ring slot; dsr_add(eth0_rx_dsr) [if not armed]
  └─ (IRQ returns)
  └─ next _task_sched() → dsr_drain() → eth0_rx_dsr
       └─ pbuf_alloc + pbuf_take + eth0.input
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
