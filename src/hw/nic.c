#include <hw/nic.h>
#include <hw/pci_list.h>
#include <lib/klib.h>
#include <macro.h>

nic_dev *nic_intel_8254x_create(uint32_t device, uint16_t v, uint16_t d);

static nic_dev network_devices[MAX_NETWORK_DEV] = { 0 };

static nic_dev *nic_register(nic_dev *dev)
{
	int i = 0;
	if (dev == NULL)
		return NULL;

	for (i = 0; i < MAX_NETWORK_DEV; i++) {
		if (network_devices[i].ven == 0 &&
		    network_devices[i].dev == 0) {
			network_devices[i] = *dev;
			return &network_devices[i];
		}
	}
	return NULL;
}

static void scan_all_pci(uint32_t device, uint16_t v, uint16_t d, void *extra)
{
	nic_dev *dev = NULL;

	if (v == 0x8086) {
		if (d == 0x100E || d == 0x100F || d == 0x1000 || d == 0x1001)
			dev = nic_intel_8254x_create(device, v, d);
	}

	if (dev) {
		if (!dev->init) {
			free(dev);
			return;
		}

		if (dev->init(dev) != 0) {
			free(dev);
			return;
		}

		nic_dev *registered = nic_register(dev);
		if (registered && registered->on_register)
			registered->on_register(registered);
		free(dev);
	}
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

static void nic_scan_all()
{
	pci_scan(scan_all_pci, PCI_SCAN_ALL, 0);
}

KERNEL_INIT(6, nic_scan_all);