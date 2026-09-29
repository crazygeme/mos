#include <device/sysfs.h>
#include <device/pci_sysfs.h>
#include <device/pci.h>
#include <driver/driver.h>
#include <lib/klib.h>
#include <errno.h>

struct sysfs_pci {
	vfs_entry_node *node;
	const device_t *hardware;
};

/* PCI attributes use register descriptors, independent of pathname lookup. */
enum {
	PCI_ATTR_VENDOR,
	PCI_ATTR_DEVICE,
	PCI_ATTR_CLASS,
	PCI_ATTR_REVISION,
	PCI_ATTR_SUBVENDOR,
	PCI_ATTR_SUBDEVICE,
	PCI_ATTR_BOOT,
	PCI_ATTR_ENABLE,
	PCI_ATTR_RESOURCES,
	PCI_ATTR_CONFIG,
	PCI_ATTR_UEVENT
};

static const struct {
	const char *name;
	unsigned offset, width;
} pci_fields[] = {
	{ "vendor", PCI_VENDOR_ID, 2 },	 { "device", PCI_DEVICE_ID, 2 },
	{ "class", PCI_REVISION_ID, 4 }, { "revision", PCI_REVISION_ID, 1 },
	{ "subsystem_vendor", 0x2c, 2 }, { "subsystem_device", 0x2e, 2 },
};

static int sysfs_pci_show(void *data, unsigned tag, char *buf,
			  unsigned capacity)
{
	struct sysfs_pci *device = data;
	unsigned value, length = 0, i;
	if (capacity < 1024)
		return -ENOSPC;
	if (tag <= PCI_ATTR_SUBDEVICE) {
		value = pci_read_field(device->hardware->address,
				       pci_fields[tag].offset,
				       pci_fields[tag].width);
		if (tag == PCI_ATTR_CLASS)
			value >>= 8;
		return sprintf(buf, "0x%06x\n", value);
	}
	switch (tag) {
	case PCI_ATTR_BOOT:
		return sprintf(buf, "%u\n", device->hardware->boot_vga);
	case PCI_ATTR_ENABLE:
		return sprintf(buf, "%u\n",
			       !!(pci_read_field(device->hardware->address,
						 PCI_COMMAND, 2) &
				  3));
	case PCI_ATTR_RESOURCES:
		for (i = 0; i < 7; i++) {
			const pci_resource *resource =
				&device->hardware->resources[i];
			uint64_t end =
				resource->size ?
					resource->start + resource->size - 1 :
					0;
			length += sprintf(buf + length,
					  "%08x%08x %08x%08x %08x%08x\n",
					  (unsigned)(resource->start >> 32),
					  (unsigned)resource->start,
					  (unsigned)(end >> 32), (unsigned)end,
					  0U, resource->flags);
		}
		return length;
	case PCI_ATTR_UEVENT:
		return sprintf(buf, "PCI_SLOT_NAME=%s\n%s%s%s",
			       vfs_entry_name(device->node),
			       device->hardware->driver ? "DRIVER=" : "",
			       device->hardware->driver ?
				       device->hardware->driver->name :
				       "",
			       device->hardware->driver ? "\n" : "");
	case PCI_ATTR_CONFIG:
		return 256;
	default:
		return -EINVAL;
	}
}

static ssize_t sysfs_pci_read(void *data, unsigned tag, void *buf, size_t size,
			      loff_t *pos)
{
	struct sysfs_pci *device = data;
	unsigned i;
	(void)tag;
	if ((uint64_t)*pos >= 256)
		return 0;
	if (size > 256 - *pos)
		size = 256 - *pos;
	for (i = 0; i < size; i++)
		((unsigned char *)buf)[i] =
			pci_read_field(device->hardware->address, *pos + i, 1);
	*pos += size;
	return size;
}

static ssize_t sysfs_pci_write(void *data, unsigned tag, const void *buf,
			       size_t size, loff_t *pos)
{
	struct sysfs_pci *device = data;
	unsigned i, command;
	if (tag == PCI_ATTR_CONFIG) {
		if ((uint64_t)*pos >= 256)
			return -EFBIG;
		if (size > 256 - *pos)
			size = 256 - *pos;
		for (i = 0; i < size; i++)
			pci_write_field(device->hardware->address, *pos + i, 1,
					((const unsigned char *)buf)[i]);
	} else if (tag == PCI_ATTR_ENABLE) {
		if (!size)
			return 0;
		if (*(const char *)buf != '0' && *(const char *)buf != '1')
			return -EINVAL;
		command = pci_read_field(device->hardware->address, PCI_COMMAND,
					 2);
		command = *(const char *)buf == '1' ? command | 3 :
						      command & ~3U;
		pci_write_field(device->hardware->address, PCI_COMMAND, 2,
				command);
	} else
		return -EACCES;
	*pos += size;
	return size;
}

static const vfs_entry_attribute_ops pci_text_ops = { .show = sysfs_pci_show };
static const vfs_entry_attribute_ops pci_enable_ops = {
	.show = sysfs_pci_show,
	.write = sysfs_pci_write
};
static const vfs_entry_attribute_ops pci_config_ops = {
	.show = sysfs_pci_show,
	.read = sysfs_pci_read,
	.write = sysfs_pci_write,
};

vfs_entry_node *pci_sysfs_create(const device_t *hardware)
{
	vfs_entry_tree *tree = device_sysfs_entries();
	struct sysfs_pci *device;
	vfs_entry_node *devices = pci_sysfs_hardware();
	unsigned address = hardware->address;
	char name[32];
	unsigned i;
	if (!devices || vfs_entry_tree_error(tree))
		return NULL;
	device = vfs_entry_tree_alloc(tree, sizeof(*device));
	if (!device)
		return NULL;
	device->hardware = hardware;
	sprintf(name, "0000:%02x:%02x.%u", pci_extract_bus(address),
		pci_extract_slot(address), pci_extract_func(address));
	device->node = vfs_entry_directory_create(devices, name);
	if (!device->node)
		return NULL;
	for (i = 0; i < sizeof(pci_fields) / sizeof(pci_fields[0]); i++)
		vfs_entry_attribute(device->node, pci_fields[i].name, 0444,
				    &pci_text_ops, device, i);
	vfs_entry_attribute(device->node, "boot_vga", 0444, &pci_text_ops,
			    device, PCI_ATTR_BOOT);
	vfs_entry_attribute(device->node, "enable", 0644, &pci_enable_ops,
			    device, PCI_ATTR_ENABLE);
	vfs_entry_attribute(device->node, "resource", 0444, &pci_text_ops,
			    device, PCI_ATTR_RESOURCES);
	vfs_entry_attribute(device->node, "config", 0644, &pci_config_ops,
			    device, PCI_ATTR_CONFIG);
	vfs_entry_attribute(device->node, "uevent", 0444, &pci_text_ops, device,
			    PCI_ATTR_UEVENT);
	vfs_entry_link(device->node, "subsystem", pci_sysfs_bus());
	for (i = 0; i < 6; i++) {
		const pci_resource *resource = &device->hardware->resources[i];
		vfs_entry_node *node;
		if (!resource->size || (resource->flags & 1) ||
		    resource->start + resource->size > 0x100000000ULL)
			continue;
		sprintf(name, "resource%u", i);
		node = vfs_entry_attribute(device->node, name, 0600, NULL,
					   device, i);
		vfs_entry_resource(node, resource->start, resource->size);
	}
	if (vfs_entry_tree_error(tree)) {
		sb_put(vfs_entry_super(device->node));
		return NULL;
	}
	return device->node;
}

vfs_entry_node *pci_sysfs_bus(void)
{
	vfs_entry_tree *tree = device_sysfs_entries();
	if (!tree)
		return NULL;
	return vfs_entry_directory(
		vfs_entry_directory(vfs_entry_root(tree), "bus"), "pci");
}

vfs_entry_node *pci_sysfs_devices(void)
{
	return vfs_entry_directory(pci_sysfs_bus(), "devices");
}

vfs_entry_node *pci_sysfs_device(unsigned address)
{
	char name[32];
	sprintf(name, "0000:%02x:%02x.%u", pci_extract_bus(address),
		pci_extract_slot(address), pci_extract_func(address));
	return vfs_entry_child(pci_sysfs_hardware(), name);
}

vfs_entry_node *pci_sysfs_hardware(void)
{
	vfs_entry_tree *tree = device_sysfs_entries();
	if (!tree)
		return NULL;
	return vfs_entry_directory(vfs_entry_directory(vfs_entry_root(tree),
						       "devices"),
				   "pci0000:00");
}

vfs_entry_node *pci_sysfs_drivers(void)
{
	return vfs_entry_directory(pci_sysfs_bus(), "drivers");
}

static void sysfs_register_pci(const device_t *device)
{
	vfs_entry_node *node, *driver;
	super_block *devices = vfs_entry_super(pci_sysfs_hardware());
	char path[32];
	int error;
	if (!devices)
		return;
	node = pci_sysfs_create(device);
	if (!node) {
		printk("sysfs: cannot create PCI entries for %x\n",
		       device->address);
		return;
	}
	sprintf(path, "/%s", vfs_entry_name(node));
	error = vfs_mount(devices, path, vfs_entry_super(node));
	if (error) {
		sb_put(vfs_entry_super(node));
		printk("sysfs: cannot mount %s (%d)\n", path, error);
		return;
	}
	vfs_entry_link(pci_sysfs_devices(), vfs_entry_name(node), node);
	if (!device->driver)
		return;
	driver = vfs_entry_child(pci_sysfs_drivers(), device->driver->name);
	if (driver) {
		vfs_entry_link(node, "driver", driver);
		vfs_entry_link(driver, vfs_entry_name(node), node);
	}
}

void pci_sysfs_register(void)
{
	const device_t *device;
	driver_t *driver;
	for (driver = driver_first(); driver; driver = driver->next)
		if (driver->bus == DEVICE_BUS_PCI)
			vfs_entry_directory(pci_sysfs_drivers(), driver->name);
	for (device = device_first(); device; device = device->next)
		if (device->bus == DEVICE_BUS_PCI)
			sysfs_register_pci(device);
}
