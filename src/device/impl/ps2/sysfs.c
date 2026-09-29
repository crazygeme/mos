#include <device/ps2.h>
#include <device/sysfs.h>
#include <driver/driver.h>
#include <lib/klib.h>

void ps2_sysfs_register(void)
{
	vfs_entry_tree *tree = device_sysfs_entries();
	vfs_entry_node *bus, *devices, *drivers, *controller;
	const device_t *device;
	driver_t *driver;
	if (!tree)
		return;
	bus = vfs_entry_directory(
		vfs_entry_directory(vfs_entry_root(tree), "bus"), "serio");
	devices = vfs_entry_directory(bus, "devices");
	drivers = vfs_entry_directory(bus, "drivers");
	for (driver = driver_first(); driver; driver = driver->next)
		if (driver->bus == DEVICE_BUS_PS2)
			vfs_entry_directory(drivers, driver->name);
	controller = NULL;
	for (device = device_first(); device; device = device->next) {
		vfs_entry_node *node, *binding;
		char name[16];
		if (device->bus != DEVICE_BUS_PS2)
			continue;
		if (!controller)
			controller = vfs_entry_directory(
				vfs_entry_directory(
					vfs_entry_directory(vfs_entry_root(tree),
							    "devices"),
					"platform"),
				"i8042");
		sprintf(name, "serio%u", device->address);
		node = vfs_entry_directory(controller, name);
		if (!node)
			return;
		vfs_entry_text(node, "description",
			       device->address == PS2_PORT_KEYBOARD ?
				       "i8042 keyboard port\n" :
				       "i8042 auxiliary port\n");
		vfs_entry_link(node, "subsystem", bus);
		vfs_entry_link(devices, name, node);
		if (!device->driver)
			continue;
		binding = vfs_entry_child(drivers, device->driver->name);
		vfs_entry_link(node, "driver", binding);
		vfs_entry_link(binding, name, node);
	}
}
