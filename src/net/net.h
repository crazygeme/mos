#ifndef _NET_NET_H
#define _NET_NET_H

#include <lwip/netif.h>
#include <stdint.h>

#define MAX_NETWORK_DEV 12

struct pbuf;

typedef int (*nic_rx_fn)(void *ctx, const uint8_t *buf, uint16_t len,
			 void *cookie);
typedef void (*nic_rx_reclaim_fn)(void *dev, void *cookie);

struct _nic_dev;

typedef struct {
	int (*init)(void *dev);
	void (*on_register)(struct _nic_dev *permanent);
	int (*send)(void *dev, const void *buf, uint16_t len);
	int (*send_pbuf)(void *dev, const struct pbuf *p);
	nic_rx_reclaim_fn rx_reclaim;
} nic_ops;

typedef struct _nic_dev {
	uint32_t pci_dev;
	uint16_t ven;
	uint16_t dev;
	uint8_t mac_addr[6];
	uint8_t ip_addr[4];
	const nic_ops *ops;
	nic_rx_fn rx_notify;
	void *rx_ctx;
	void *ctx;
} nic_dev;

nic_dev *nic_register_device(nic_dev *dev);
nic_dev *nic_getdev(int index);

typedef struct {
	unsigned long rx_bytes;
	unsigned long rx_packets;
	unsigned long tx_bytes;
	unsigned long tx_packets;
} net_stats_t;

void net_init(void);
/* Requires network core ownership. */
void net_set_interface_up(struct netif *nif, int up);
struct netif *net_get_default_netif(void);
void net_get_stats(net_stats_t *s);

#endif
