#include <driver/driver.h>
#include <fs/sysfs.h>

/* PC motherboard CMOS RTC occupies fixed index/data ports. */
static device_t cmos_rtc = { .bus = DEVICE_BUS_PLATFORM, .address = 0x70 };
static unsigned scanned;

void platform_scan(void)
{
	vfs_entry_tree *tree;
	vfs_entry_node *bus, *devices, *node;
	if (scanned)
		return;
	scanned = 1;
	device_register(&cmos_rtc);
	tree = device_sysfs_entries();
	if (!tree)
		return;
	bus = driver_bus_entry(DEVICE_BUS_PLATFORM);
	devices = vfs_entry_directory(vfs_entry_directory(vfs_entry_root(tree),
							  "devices"),
				      "platform");
	node = vfs_entry_directory(devices, "cmos-rtc");
	cmos_rtc.sysfs_entry = node;
	vfs_entry_link(node, "subsystem", bus);
	vfs_entry_link(vfs_entry_directory(bus, "devices"), "cmos-rtc", node);
}
