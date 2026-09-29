#include <device/device.h>
#include <driver/driver.h>
#include <macro.h>
#include <errno.h>

static device_t *devices;
static device_t **device_tail = &devices;
static unsigned boot_vga;

const device_t *device_first(void)
{
	return devices;
}

void device_probe(device_t *device)
{
	driver_t *driver = device->selected_driver;
	const pci_device_id *id;
	if (!driver || device->probe_done)
		return;
	device->probe_done = 1;
	device->probe_error = -ENODEV;
	if (device->bus != driver->bus)
		return;
	switch (device->bus) {
	case DEVICE_BUS_PCI:
		id = driver_match_pci(driver, device);
		if (id)
			device->probe_error = driver->probe_pci(
				device->address, device->vendor_id,
				device->device_id, id);
		break;
	case DEVICE_BUS_PS2:
		if (driver->probe_ps2 && driver->ps2_port == device->address)
			device->probe_error =
				driver->probe_ps2(device->address);
		break;
	default:
		break;
	}
	if (!device->probe_error)
		device->driver = driver;
}

void device_register(device_t *device)
{
	device_t *existing;
	for (existing = devices; existing; existing = existing->next)
		if (existing->bus == device->bus &&
		    existing->address == device->address)
			return;
	device->next = 0;
	*device_tail = device;
	device_tail = &device->next;
	if (device->bus == DEVICE_BUS_PCI && !boot_vga &&
	    device->type == 0x0300)
		device->boot_vga = boot_vga = 1;
	device->selected_driver = driver_select(device);
	if (!device->selected_driver)
		return;
	if (device->bus == DEVICE_BUS_PCI &&
	    device->selected_driver->console_init)
		device->selected_driver->console_init(device->address);
	if (device->selected_driver->early)
		device_probe(device);
}

static void devices_init(void)
{
	device_t *device;
	for (device = devices; device; device = device->next)
		device_probe(device);
}

/* Disk discovery must finish before the root filesystem mounts at level 3. */
KERNEL_INIT(2, devices_init);
