#include <fs/sysfs.h>
#include <driver/driver.h>

static device_t virtual_devices[VDEV_COUNT];
static unsigned scanned;
static const char *const names[] = {
	"tty", "pty", "ptmx", "loop", "mem", "null", "zero", "random",
	"kmsg", "initctl", "fd"
};
_Static_assert(sizeof(names) / sizeof(names[0]) == VDEV_COUNT,
	       "Every virtual device must have a name");

void virtual_scan(void)
{
	if (scanned)
		return;
	scanned = 1;
	vfs_entry_tree *tree = device_sysfs_entries();
	vfs_entry_node *bus = driver_bus_entry(DEVICE_BUS_VIRTUAL);
	vfs_entry_node *parent =
		tree ? vfs_entry_directory(
			       vfs_entry_directory(vfs_entry_root(tree),
						   "devices"),
			       "virtual") :
		       NULL;
	for (unsigned i = 0; i < VDEV_COUNT; i++) {
		virtual_devices[i].bus = DEVICE_BUS_VIRTUAL;
		virtual_devices[i].address = i;
		device_register(&virtual_devices[i]);
		virtual_devices[i].sysfs_entry =
			vfs_entry_directory(parent, names[i]);
		vfs_entry_link(virtual_devices[i].sysfs_entry, "subsystem",
			       bus);
		vfs_entry_link(vfs_entry_directory(bus, "devices"), names[i],
			       virtual_devices[i].sysfs_entry);
	}
}
