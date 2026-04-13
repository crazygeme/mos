# Network Stack

**Source:** `src/net/net.c`, `src/net/sock.c`, `src/net/sock_cb.c`, `src/net/sock_msg.c`, `src/net/sock_opt.c`, `src/net/sock_un.c`, `src/hw/nic.c`, `src/hw/nic_intel_8254x.c`, `src/proc/net/*`, `src/syscall/syscall_net.c`

## Status

The current tree has a working IPv4 socket stack built on lwIP in `NO_SYS` mode, plus a local AF_UNIX implementation.

Implemented today:

- Loopback is always available.
- One physical NIC (`eth0`) can be attached through the Intel 8254x driver.
- DHCP is started automatically for `eth0` when a NIC exists.
- `/proc/net/dev`, `/proc/net/route`, `/proc/net/arp`, and `/proc/net/if_inet6` are present.
- AF_INET supports `SOCK_STREAM`, `SOCK_DGRAM`, and `SOCK_RAW`.
- AF_UNIX supports `SOCK_STREAM`, `SOCK_DGRAM`, `socketpair()`, and `SCM_RIGHTS`.
- `sendmsg()` and `recvmsg()` are implemented, including selected control messages.

Not implemented or intentionally partial:

- IPv6 is not implemented; `/proc/net/if_inet6` is empty by design.
- The stack is IPv4-only.
- The socket layer is synchronous and timeout-based; there are no kernel networking threads.

## Architecture

The stack is layered like this:

```text
user space
  -> sys_socketcall
  -> src/net/sock*.c
  -> lwIP (NO_SYS)
  -> src/net/net.c netif glue
  -> src/hw/nic*.c
```

Key design choice: lwIP runs in `NO_SYS` mode, so all protocol work happens from:

- deferred receive handling
- blocking socket syscalls
- lwIP timer callbacks

There is no dedicated network thread.

## Boot and Interface Bring-Up

### Loopback

`lwip_init()` is called before probing for a physical NIC. That means the loopback netif created by lwIP is available even when no PCI NIC is present.

If no NIC is found, MOS logs:

- `net: no NIC found, loopback only`

### `eth0`

When `nic_getdev(0)` succeeds:

- MOS wraps it in a lwIP `netif`
- registers it as the default interface
- marks it up
- connects driver RX callbacks to the net stack
- starts DHCP

The exported default netif from `net_get_default_netif()` is the physical `eth0` object, not loopback.

## NIC Driver Status

Current in-tree hardware support is the Intel 8254x family in `src/hw/nic_intel_8254x.c`.

Known supported IDs:

- `8086:100E`
- `8086:100F`
- `8086:1000`
- `8086:1001`

This matches the QEMU `e1000` device used by the project.

Driver model:

- TX is descriptor-based and effectively synchronous from the caller's point of view.
- RX is interrupt-driven.
- MMIO is used for device registers.
- DMA buffers live in kernel virtual memory with physical addresses derived from the kernel mapping.

## Receive Path

The receive path is deliberately split in two.

### Phase 1: IRQ-safe enqueue

The NIC ISR calls `rx_notify`, which is wired to `eth0_rx_enqueue()`.

That function:

- copies the raw frame into a fixed 32-slot ring
- does not allocate memory
- arms a deferred service routine once

This avoids deadlocking against the heap allocator from interrupt context.

### Phase 2: DSR to lwIP

`eth0_rx_dsr()` runs later from the DSR layer:

- allocates a lwIP `pbuf`
- copies the frame into it
- feeds it into `eth0.input()`

Counters updated here:

- `rx_bytes`
- `rx_packets`

If the IRQ ring is full, frames are dropped.

## Transmit Path

`eth0_linkoutput()` is the lwIP `linkoutput` hook.

Current behavior:

- flattens the lwIP `pbuf` chain into a static bounce buffer
- calls the NIC driver's `send`
- updates `tx_bytes` and `tx_packets` on success

The code assumes non-reentrant task/DSR use of that static bounce buffer.

## Timer and Timeout Pump

lwIP timers are driven through the shared kernel timer services:

- `sys_now()` returns `time_now_ms()`
- `ps_start_system_services()` is started after `lwip_init()`
- the periodic service task calls both `sys_check_timeouts()` and `netif_poll_all()`

That timeout pump drives:

- DHCP retransmits
- ARP expiration
- TCP retransmits and keepalive-related lwIP timers

## AF_INET Socket Support

The `socketcall` entry point in `src/syscall/syscall_net.c` dispatches to the in-kernel socket layer.

Supported syscalls:

- `socket`
- `bind`
- `connect`
- `listen`
- `accept` and `accept4`
- `getsockname`
- `getpeername`
- `socketpair` for AF_UNIX only
- `send`, `recv`
- `sendto`, `recvfrom`
- `sendmsg`, `recvmsg`
- `shutdown`
- `setsockopt`, `getsockopt`

All socket objects are exposed through the VFS as `S_IFSOCK` files.

### TCP

Current TCP behavior:

- `connect()` is asynchronous underneath but exposed as a blocking syscall with timeout.
- `listen()` uses lwIP's listen PCB with `SOCK_ACCEPT_BACKLOG`.
- accepted connections get their own `mos_sock`.
- receive data is byte-stream data stored in the per-socket ring buffer.
- if the userspace receive ring cannot hold an entire incoming segment, the lwIP callback returns `ERR_MEM` so lwIP retries later instead of truncating the stream.
- EOF is surfaced by transitioning to `SS_DISCONNECTING`.

Writes:

- use `tcp_write(..., TCP_WRITE_FLAG_COPY)`
- block until send buffer space exists unless `O_NONBLOCK` or `MSG_DONTWAIT` is used

### UDP

Current UDP behavior:

- incoming datagrams are stored as `u16 length + payload` in the receive ring
- `recv`/`recvfrom` return one datagram at a time
- truncation discards the unread tail of that datagram
- source address, destination address, interface index, and TTL are captured for `recvmsg()` ancillary data

### Raw IPv4

Two raw modes exist:

- `SOCK_RAW + IPPROTO_ICMP`
- `SOCK_RAW + IPPROTO_RAW`

`IPPROTO_ICMP`:

- uses lwIP raw PCBs
- receives full IP packet data starting at the IP header
- supports `ICMP_FILTER`
- leaves packets unconsumed so lwIP can still answer echo requests, which is required for loopback ping

`IPPROTO_RAW`:

- enables `hdrincl` by default
- transmits a fully formed IPv4 packet supplied by userspace
- routes through `ip4_route()` and `netif->output()`

## AF_UNIX Socket Support

AF_UNIX is implemented entirely in `src/net/sock_un.c`, separate from lwIP.

Supported features:

- pathname sockets via `bind(path)`
- `listen` and `accept`
- `SOCK_STREAM`
- `SOCK_DGRAM`
- `socketpair`
- descriptor passing with `SCM_RIGHTS`

Current design:

- the namespace is a small flat kernel table keyed by resolved path
- `bind()` creates a socket inode with `vfs_mknod(..., S_IFSOCK, ...)`
- stream `connect()` creates the server-side accepted socket immediately and queues it on the listener
- datagram Unix sockets connect directly by remembering the peer pointer

Concurrency note:

- AF_UNIX uses `rxbuf_lock` because after `fork()` the same socket object can be referenced by multiple tasks concurrently
- the same lock also serializes ancillary fd queues for `SCM_RIGHTS`

## `sendmsg`, `recvmsg`, and Ancillary Data

`sendmsg()` and `recvmsg()` are implemented for AF_INET and AF_UNIX.

### Receive-side cmsgs

When enabled through `setsockopt()`, `recvmsg()` can append:

- `SOL_SOCKET / SO_TIMESTAMP` as `struct timeval`
- `IPPROTO_IP / IP_TTL` as `int`
- `IPPROTO_IP / IP_PKTINFO` as `struct in_pktinfo`

For AF_UNIX, `recvmsg()` can also return:

- `SOL_SOCKET / SCM_RIGHTS`

### Supported flags

The current receive paths meaningfully handle:

- `MSG_DONTWAIT`
- `MSG_TRUNC` as a returned condition when a datagram is larger than the supplied iovecs

`MSG_CTRUNC` is reported when the supplied control buffer is too small.
`MSG_PEEK` is not implemented yet in the current receive path.

## Socket Options

Implemented options include:

### `SOL_SOCKET`

- `SO_REUSEADDR`
- `SO_KEEPALIVE` for TCP
- `SO_BROADCAST` for UDP
- `SO_TYPE`
- `SO_ERROR`
- `SO_RCVBUF`
- `SO_SNDBUF`
- `SO_TIMESTAMP`

Some options are accepted as stubs but do not currently change behavior:

- `SO_LINGER`
- `SO_RCVTIMEO`
- `SO_SNDTIMEO`

### IP / ICMP / RAW

- `IP_HDRINCL`
- `IP_TTL` as a recvmsg cmsg toggle
- `IP_PKTINFO` as a recvmsg cmsg toggle
- `ICMP_FILTER`
- `IP_RECVERR` is currently accepted as a no-op

### TCP

- `TCP_NODELAY`
- `TCP_KEEPIDLE` is currently accepted as a stub

## Blocking and Wakeups

The socket layer uses a simple waiter model:

- a blocking call stores the current task in `sk->waiter`
- lwIP callbacks or peer activity call `sock_wakeup()`
- poll/select waiters are also nudged through `poll_task`

Current user-visible behavior:

- blocking operations time out after `SOCK_TIMEOUT_MS`
- `O_NONBLOCK` and `MSG_DONTWAIT` return `-EAGAIN` where supported
- pending signals break waits with `-EINTR`

## `/proc/net`

The proc layer currently exports:

- `/proc/net/dev`
- `/proc/net/route`
- `/proc/net/arp`
- `/proc/net/if_inet6`

Current contents:

- `/proc/net/dev` shows loopback plus the live counters for `eth0`
- `/proc/net/route` shows a default route and subnet route once `eth0` has a non-zero IPv4 address
- `/proc/net/arp` walks lwIP's ARP table
- `/proc/net/if_inet6` is empty because IPv6 is not implemented

These files exist mainly to satisfy common userland tools such as `ifconfig` and routing utilities.

## Socket ioctls

The socket file implementation also exposes Linux-style ioctls used by common networking tools.

Implemented today:

- `SIOCGSTAMP`
- `FIONREAD`
- `FIONBIO` as a compatibility no-op
- `SIOCGIFCONF`
- `SIOCGIFFLAGS`
- `SIOCGIFADDR`
- `SIOCGIFNETMASK`
- `SIOCGIFBRDADDR`
- `SIOCGIFHWADDR`
- `SIOCGIFMTU`
- `SIOCGIFINDEX`
- `SIOCGIFMETRIC`

The interface view currently consists of:

- loopback as `lo`
- the default physical interface as the lwIP-derived name for `eth0`

## Limitations and Notes

- IPv4 only.
- Only the first registered physical NIC is attached to the lwIP stack.
- RX uses a fixed-size 32-frame staging ring before lwIP; overflow drops frames.
- TCP and UDP buffering is intentionally simple and bounded by the socket ring sizes.
- Many socket options are partial compatibility shims rather than full Linux behavior.
- The implementation is designed for correctness and compatibility with the current userland, not for SMP scalability or high throughput.
