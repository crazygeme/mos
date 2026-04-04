/*
 * lwipopts.h - lwIP configuration for MOS kernel.
 *
 * Bare-metal / NO_SYS configuration: the OS-abstraction layer is
 * provided by the kernel itself.  The socket / netconn APIs are
 * disabled until the threading glue is wired up.
 */
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

/* ── Suppress unavailable system headers ────────────────────────────────── */
#define LWIP_NO_INTTYPES_H 1 /* no inttypes.h in kernel environment  */
#define LWIP_NO_LIMITS_H 1 /* no limits.h in kernel environment    */
#define LWIP_NO_CTYPE_H 1 /* no ctype.h in kernel environment     */
#define LWIP_NO_STDINT_H 0 /* stdint.h is provided                 */
#define LWIP_NO_STDDEF_H 0 /* stddef.h is provided                 */

/* ── OS integration ─────────────────────────────────────────────────────── */
#define NO_SYS 1 /* no OS: use raw/callback API only    */
#define SYS_LIGHTWEIGHT_PROT 0 /* no SYS_ARCH_PROTECT needed yet      */

/* ── Memory ─────────────────────────────────────────────────────────────── */
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEM_SIZE (64 * 1024)
#define PBUF_POOL_SIZE 64
#define PBUF_POOL_BUFSIZE 1600

/* ── TCP tuning ──────────────────────────────────────────────────────────── */
#define TCP_MSS 1460
#define TCP_WND (32 * 1024)

/* ── Netconn / Socket API (require OS threading) ────────────────────────── */
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0

/* ── Protocol support ───────────────────────────────────────────────────── */
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_ICMP 1
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_ARP 1
#define LWIP_RAW 1

/* ── Ethernet ───────────────────────────────────────────────────────────── */
#define LWIP_ETHERNET 1

/* ── Loopback ───────────────────────────────────────────────────────────── */
/* Enables per-netif loopback queue and the dedicated lo netif (127.0.0.1). */
/* LWIP_HAVE_LOOPIF is derived as (LWIP_NETIF_LOOPBACK && !LWIP_SINGLE_NETIF). */
/* In NO_SYS mode, LWIP_NETIF_LOOPBACK_MULTITHREADING defaults to 0 (poll). */
#define LWIP_NETIF_LOOPBACK 1

/* ── Disable heavyweight optional features ──────────────────────────────── */
#define LWIP_NETIF_API 0
#define LWIP_SLIP 0
#define PPP_SUPPORT 0
#define LWIP_IPV6 0
#define LWIP_IPV6_MLD 0
#define LWIP_MULTICAST_TX_OPTIONS 0

/* ── Random number source ───────────────────────────────────────────────── */
/* Provide a simple stub; the kernel can replace this later. */
#define LWIP_RAND() (0x12345678UL)

/* ── Statistics / debug ─────────────────────────────────────────────────── */
#define LWIP_STATS 0
#define LWIP_DEBUG 0

/* ── altcp ───────────────────────────────────────────────────────────────── */
#define LWIP_ALTCP 0

/* ── Checksum offload ───────────────────────────────────────────────────── */
#define CHECKSUM_BY_HARDWARE 0

#endif /* _LWIPOPTS_H */
