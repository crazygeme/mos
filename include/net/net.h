#ifndef _NET_NET_H
#define _NET_NET_H

#include <lwip/netif.h>

typedef struct {
	unsigned long rx_bytes;
	unsigned long rx_packets;
	unsigned long tx_bytes;
	unsigned long tx_packets;
} net_stats_t;

void net_init(void);
struct netif *net_get_default_netif(void);
void net_get_stats(net_stats_t *s);

#endif
