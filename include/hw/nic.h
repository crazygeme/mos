#ifndef _HW_NIC_H
#define _HW_NIC_H
#include <hw/pci.h>

#define MAX_NETWORK_DEV 12

struct pbuf;

typedef int (*nic_rx_fn)(void *ctx, const uint8_t *buf, uint16_t len,
			 void *cookie);
typedef void (*nic_rx_reclaim_fn)(void *dev, void *cookie);

typedef struct _nic_dev {
	uint32_t pci_dev;
	uint16_t ven;
	uint16_t dev;
	uint8_t mac_addr[6];
	uint8_t ip_addr[4];
	int (*init)(void *dev);
	void (*on_register)(
		struct _nic_dev
			*permanent); /* called after copy into device table */
	int (*send)(void *dev, const void *buf, uint16_t len);
	int (*send_pbuf)(void *dev, const struct pbuf *p);
	nic_rx_fn rx_notify;
	nic_rx_reclaim_fn rx_reclaim;
	void *rx_ctx;
	void *ctx;
} nic_dev;

nic_dev *nic_getdev(int index);
nic_dev *nic_getdev_by_mac(uint8_t *mac);
nic_dev *nic_getdev_by_ip(uint8_t *ip);

#endif
