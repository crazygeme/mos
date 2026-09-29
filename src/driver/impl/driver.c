#include <driver/driver.h>
#include <device/device.h>
#include <lib/klib.h>

static driver_t *driver_list;
extern driver_t *const __driver_start[];
extern driver_t *const __driver_end[];

void driver_register(driver_t *driver)
{
	driver_t **tail = &driver_list;
	if (!driver)
		return;
	while (*tail) {
		if (*tail == driver)
			return;
		tail = &(*tail)->next;
	}
	driver->next = 0;
	*tail = driver;
}

void drivers_init(void)
{
	driver_t *const *driver;
	for (driver = __driver_start; driver < __driver_end; driver++)
		driver_register(*driver);
}

driver_t *driver_first(void)
{
	return driver_list;
}

const pci_device_id *driver_match_pci(const driver_t *driver,
				      const device_t *device)
{
	unsigned i;
	if (device->bus != DEVICE_BUS_PCI || driver->bus != device->bus ||
	    !driver->probe_pci || !driver->pci_ids)
		return 0;
	for (i = 0; i < driver->pci_id_count; i++) {
		const pci_device_id *id = &driver->pci_ids[i];
		if ((id->vendor_id == PCI_ANY_ID ||
		     id->vendor_id == device->vendor_id) &&
		    (id->device_id == PCI_ANY_ID ||
		     id->device_id == device->device_id) &&
		    !((id->class_code ^ device->type) & id->class_mask))
			return id;
	}
	return 0;
}

driver_t *driver_select(const device_t *device)
{
	driver_t *driver, *selected = NULL;
	for (driver = driver_list; driver; driver = driver->next) {
		if (!driver_match_pci(driver, device) &&
		    !(device->bus == DEVICE_BUS_PS2 &&
		      driver->bus == device->bus && driver->probe_ps2 &&
		      driver->ps2_port == device->address))
			continue;
		if (selected) {
			printk("device: conflicting drivers %s and %s for bus %u address %x\n",
			       selected->name, driver->name,
			       (unsigned)device->bus, device->address);
			return NULL;
		}
		selected = driver;
	}
	return selected;
}
