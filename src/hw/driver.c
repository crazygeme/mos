#include <hw/driver.h>
#include <hw/pci.h>
#include <lib/klib.h>
#include <macro.h>

static hw_driver *hw_drivers;

void hw_driver_register(hw_driver *driver)
{
	if (!driver)
		return;

	driver->next = hw_drivers;
	hw_drivers = driver;
}

int hw_probe_pci(uint32_t device, uint16_t vendor_id, uint16_t device_id)
{
	hw_driver *driver;
	int matched = 0;

	for (driver = hw_drivers; driver; driver = driver->next) {
		unsigned i;

		if (driver->bus != HW_BUS_PCI || !driver->probe_pci)
			continue;

		for (i = 0; i < driver->pci_id_count; i++) {
			const hw_pci_id *id = &driver->pci_ids[i];

			if (id->vendor_id != vendor_id ||
			    id->device_id != device_id)
				continue;

			if (driver->probe_pci(device, vendor_id, device_id,
					      id) == 0)
				matched++;
		}
	}

	return matched;
}

static void hw_pci_probe_one(uint32_t device, uint16_t vendor_id,
			     uint16_t device_id, void *extra)
{
	(void)extra;
	hw_probe_pci(device, vendor_id, device_id);
}

void hw_pci_probe_all(void)
{
	pci_scan(hw_pci_probe_one, PCI_SCAN_ALL, NULL);
}

KERNEL_INIT(6, hw_pci_probe_all);
