#include <driver/driver.h>
#include <device/device.h>
#include <lib/klib.h>
#include <errno.h>

static driver_t *driver_list;
static driver_t **driver_tail = &driver_list;
extern driver_t *const __driver_start[];
extern driver_t *const __driver_end[];

void driver_register(driver_t *driver)
{
	if (!driver || driver->registered)
		return;
	driver->registered = 1;
	driver->next = 0;
	*driver_tail = driver;
	driver_tail = &driver->next;
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

static int pci_match(const driver_t *driver, device_t *device)
{
	const pci_device_id *id = driver_match_pci(driver, device);
	if (!id)
		return 0;
	device->matched_pci_id = id;
	return 1;
}

static int ps2_match(const driver_t *driver, device_t *device)
{
	return driver->probe_ps2 && driver->ps2_port == device->address;
}

static int pci_probe(device_t *device)
{
	return device->selected_driver->probe_pci(device->address,
						  device->vendor_id,
						  device->device_id,
						  device->matched_pci_id);
}

static int ps2_probe(device_t *device)
{
	return device->selected_driver->probe_ps2(device->address);
}

static const struct driver_bus_ops {
	int (*match)(const driver_t *, device_t *);
	int (*probe)(device_t *);
} bus_ops[] = {
	[DEVICE_BUS_PCI] = { pci_match, pci_probe },
	[DEVICE_BUS_USB] = { 0 },
	[DEVICE_BUS_PS2] = { ps2_match, ps2_probe },
};

driver_t *driver_select(device_t *device)
{
	driver_t *driver, *selected = NULL;
	device->matched_pci_id = NULL;
	if ((unsigned)device->bus >= sizeof(bus_ops) / sizeof(bus_ops[0]) ||
	    !bus_ops[device->bus].match)
		return NULL;
	for (driver = driver_list; driver; driver = driver->next) {
		if (driver->bus != device->bus ||
		    !bus_ops[device->bus].match(driver, device))
			continue;
		if (selected) {
			printk("device: conflicting drivers %s and %s for bus %u address %x\n",
			       selected->name, driver->name,
			       (unsigned)device->bus, device->address);
			device->matched_pci_id = NULL;
			return NULL;
		}
		selected = driver;
	}
	return selected;
}

int driver_probe(device_t *device)
{
	if (!device->selected_driver ||
	    device->selected_driver->bus != device->bus ||
	    (unsigned)device->bus >= sizeof(bus_ops) / sizeof(bus_ops[0]) ||
	    !bus_ops[device->bus].probe)
		return -ENODEV;
	return bus_ops[device->bus].probe(device);
}
