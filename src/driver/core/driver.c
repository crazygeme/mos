#include <fs/sysfs.h>
#include <driver/driver.h>
#include <lib/klib.h>
#include <errno.h>
#include <macro.h>

static list_entry driver_list = { &driver_list, &driver_list };
extern driver_t *const __driver_start[];
extern driver_t *const __driver_end[];

vfs_entry_node *driver_bus_entry(device_bus_t bus)
{
	static const char *const names[] = { "pci", "usb", "serio", "virtual",
					     "platform" };
	vfs_entry_tree *tree = device_sysfs_entries();
	if (!tree || (unsigned)bus >= sizeof(names) / sizeof(names[0]))
		return NULL;
	return vfs_entry_directory(
		vfs_entry_directory(vfs_entry_root(tree), "bus"), names[bus]);
}

vfs_entry_node *driver_device_entry(device_bus_t bus, uint32_t address)
{
	const device_t *device = device_find(bus, address);
	return device ? device->sysfs_entry : NULL;
}

void driver_register(driver_t *driver)
{
	if (!driver || driver->registered)
		return;
	driver->registered = 1;
	list_insert_tail(&driver_list, &driver->list);
	if (driver->name)
		driver->sysfs_entry = vfs_entry_directory(
			vfs_entry_directory(driver_bus_entry(driver->bus),
					    "drivers"),
			driver->name);
}

void drivers_init(void)
{
	driver_t *const *driver;
	for (driver = __driver_start; driver < __driver_end; driver++)
		driver_register(*driver);
}

driver_t *driver_first(void)
{
	return list_is_empty(&driver_list) ?
		       NULL :
		       container_of(driver_list.next, driver_t, list);
}

driver_t *driver_next(const driver_t *driver)
{
	return driver->list.next == &driver_list ?
		       NULL :
		       container_of(driver->list.next, driver_t, list);
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

static int virtual_match(const driver_t *driver, device_t *device)
{
	return driver->probe_virtual && driver->virtual_id == device->address;
}

static int virtual_probe(device_t *device)
{
	return device->selected_driver->probe_virtual();
}

static int platform_match(const driver_t *driver, device_t *device)
{
	return driver->probe_platform &&
	       driver->platform_address == device->address;
}

static int platform_probe(device_t *device)
{
	return device->selected_driver->probe_platform(device->address);
}

static const struct driver_bus_ops {
	int (*match)(const driver_t *, device_t *);
	int (*probe)(device_t *);
} bus_ops[] = {
	[DEVICE_BUS_PCI] = { pci_match, pci_probe },
	[DEVICE_BUS_USB] = { 0 },
	[DEVICE_BUS_PS2] = { ps2_match, ps2_probe },
	[DEVICE_BUS_VIRTUAL] = { virtual_match, virtual_probe },
	[DEVICE_BUS_PLATFORM] = { platform_match, platform_probe },
};

driver_t *driver_select(device_t *device)
{
	driver_t *driver, *selected = NULL;
	device->matched_pci_id = NULL;
	if ((unsigned)device->bus >= sizeof(bus_ops) / sizeof(bus_ops[0]) ||
	    !bus_ops[device->bus].match)
		return NULL;
	for (driver = driver_first(); driver; driver = driver_next(driver)) {
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

void driver_bind(device_t *device)
{
	driver_t *driver = device->selected_driver;
	if (!driver || device->probe_done)
		return;
	device->probe_done = 1;
	device->probe_error = driver_probe(device);
	if (!device->probe_error) {
		device->driver = driver;
		if (device->sysfs_entry && driver->sysfs_entry) {
			vfs_entry_link(device->sysfs_entry, "driver",
				       driver->sysfs_entry);
			vfs_entry_link(driver->sysfs_entry,
				       vfs_entry_name(device->sysfs_entry),
				       device->sysfs_entry);
		}
	}
}

static void driver_report_boot_devices(void)
{
	device_t *device;
	if (!printk_console_ready())
		return;
	for (device = (device_t *)device_first(); device;
	     device = (device_t *)device_next(device)) {
		char *information;
		char result[40];
		driver_t *driver = device->selected_driver;
		const char *name;
		if (!driver || !device->probe_done || device->boot_logged)
			continue;
		information = name_get();
		if (!information)
			return;
		name = device->sysfs_entry ?
			       vfs_entry_name(device->sysfs_entry) :
			       "unnamed";
		switch (device->bus) {
		case DEVICE_BUS_PCI:
			sprintf(information,
				"device: pci %04x:%02x:%02x.%u vendor=%04x device=%04x class=%04x",
				0, pci_extract_bus(device->address),
				pci_extract_slot(device->address),
				pci_extract_func(device->address),
				device->vendor_id, device->device_id,
				device->type);
			break;
		case DEVICE_BUS_PS2:
			sprintf(information, "device: ps2 port=%u (%s)",
				device->address,
				device->address == PS2_PORT_KEYBOARD ?
					"keyboard" :
					"mouse");
			break;
		case DEVICE_BUS_VIRTUAL:
			sprintf(information, "device: virtual %s id=%u", name,
				device->address);
			break;
		case DEVICE_BUS_PLATFORM:
			sprintf(information, "device: platform %s address=%x",
				name, device->address);
			break;
		default:
			sprintf(information, "device: bus=%u address=%x",
				device->bus, device->address);
			break;
		}
		if (device->probe_error)
			sprintf(result, "load failed: %d", device->probe_error);
		else
			strcpy(result, "loaded");
		/* One printk holds the console lock across both lines. */
		printk("%s\n        |- driver: %s (%s)\n", information,
		       driver->name ? driver->name : "unnamed", result);
		name_put(information);
		device->boot_logged = 1;
	}
}

/* Discovery only records hardware. Matching and probing belong to drivers. */
static void drivers_bind_devices(unsigned early_only)
{
	device_t *device;
	for (device = (device_t *)device_first(); device;
	     device = (device_t *)device_next(device)) {
		if (!device->match_done) {
			device->match_done = 1;
			device->selected_driver = driver_select(device);
			if (device->selected_driver &&
			    device->bus == DEVICE_BUS_PCI &&
			    device->selected_driver->console_init)
				device->selected_driver->console_init(
					device->address);
		}
		if (device->selected_driver &&
		    (!early_only || device->selected_driver->early))
			driver_bind(device);
	}
	driver_report_boot_devices();
}

void drivers_bind_early(void)
{
	drivers_bind_devices(1);
}

static void drivers_probe_remaining(void)
{
	drivers_bind_devices(0);
}

/* Disk drivers must finish before root mounting at level 3. */
KERNEL_INIT(2, drivers_probe_remaining);

void drivers_start_workers(void)
{
	const device_t *device;
	for (device = device_first(); device; device = device_next(device)) {
		driver_t *driver = device->driver;
		if (!driver || !driver->start || driver->started)
			continue;
		driver->started = 1;
		driver->start();
	}
}
