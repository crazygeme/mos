#ifndef _HW_NIC_H
#define _HW_NIC_H
#include <hw/pci.h>

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
nic_dev *nic_getdev_by_mac(uint8_t *mac);
nic_dev *nic_getdev_by_ip(uint8_t *ip);

#endif
