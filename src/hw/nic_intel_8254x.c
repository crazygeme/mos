#include <hw/nic.h>
#include <lib/klib.h>

static int nic_intel_8254x_init(void *dev)
{
	return 0;
}

static int nic_intel_8254x_uninit(void *dev)
{
	return 0;
}

static int nic_intel_8254x_up(void *dev)
{
	return 0;
}

static int nic_intel_8254x_down(void *dev)
{
	return 0;
}

nic_dev *nic_intel_8254x_create(uint32_t device, uint16_t v, uint16_t d)
{
	nic_dev *dev = malloc(sizeof(*dev));
	dev->pci_dev = device;
	dev->ven = v;
	dev->dev = d;
	memset(dev->mac_addr, 0, 6);
	memset(dev->ip_addr, 0, 6);
	dev->init = nic_intel_8254x_init;
	dev->uninit = nic_intel_8254x_uninit;
	dev->up = nic_intel_8254x_up;
	dev->down = nic_intel_8254x_down;
	dev->ctx = NULL;
	return dev;
}
