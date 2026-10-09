#include <net/net.h>
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

