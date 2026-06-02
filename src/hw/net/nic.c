#include <hw/nic.h>
#include <lib/klib.h>

static nic_dev network_devices[MAX_NETWORK_DEV] = { 0 };

nic_dev *nic_register_device(nic_dev *dev)
{
	int i = 0;
	if (dev == NULL)
		return NULL;
	if (!dev->ops || !dev->ops->init)
		return NULL;

	if (dev->ops->init(dev) != 0)
		return NULL;

	for (i = 0; i < MAX_NETWORK_DEV; i++) {
		if (network_devices[i].ven == 0 &&
		    network_devices[i].dev == 0) {
			network_devices[i] = *dev;
			if (network_devices[i].ops->on_register)
				network_devices[i].ops->on_register(
					&network_devices[i]);
			return &network_devices[i];
		}
	}
	return NULL;
}

nic_dev *nic_getdev(int index)
{
	if (index < 0 || index >= MAX_NETWORK_DEV)
		return NULL;
	if (network_devices[index].ven == 0 && network_devices[index].dev == 0)
		return NULL;
	return &network_devices[index];
}

nic_dev *nic_getdev_by_mac(uint8_t *mac)
{
	int i = 0;
	for (i = 0; i < MAX_NETWORK_DEV; i++) {
		if (memcmp(network_devices[i].mac_addr, mac, 6) == 0) {
			return &network_devices[i];
		}
	}
	return NULL;
}

nic_dev *nic_getdev_by_ip(uint8_t *ip)
{
	int i = 0;
	for (i = 0; i < MAX_NETWORK_DEV; i++) {
		if (memcmp(network_devices[i].ip_addr, ip, 6) == 0) {
			return &network_devices[i];
		}
	}
	return NULL;
}
