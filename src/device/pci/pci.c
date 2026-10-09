#include <driver/driver.h>
#include <fs/sysfs.h>
static vfs_entry_node *pci_sysfs_create(const device_t *hardware);
static vfs_entry_node *pci_sysfs_bus(void);
static vfs_entry_node *pci_sysfs_devices(void);
static vfs_entry_node *pci_sysfs_hardware(void);
/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2011-2014 Kevin Lange
 *
 * ToAruOS PCI Initialization
 */

#include <int/int.h>
#include <driver/driver.h>

#include <mm/mm.h>

//  NOTE that the 0xFFFF of 0xFF entries at the end of some tables below are
//  not properly list terminators, but are actually the printable definitions
//  of values that are legitimately found on the PCI bus.  The size
//  definitions should be used for loop control when the table is searched.

typedef struct _PCI_VENTABLE {
	unsigned short VenId;
	const char *VenShort;
	const char *VenFull;
} PCI_VENTABLE, *PPCI_VENTABLE;

extern PCI_VENTABLE PciVenTable[];
extern int PCI_VENTABLE_LEN;

typedef struct _PCI_DEVTABLE {
	unsigned short VenId;
	unsigned short DevId;
	const char *Chip;
	const char *ChipDesc;
} PCI_DEVTABLE, *PPCI_DEVTABLE;

extern PCI_DEVTABLE PciDevTable[];
extern int PCI_DEVTABLE_LEN;

typedef struct _PCI_CLASSCODETABLE {
	unsigned char BaseClass;
	unsigned char SubClass;
	unsigned char ProgIf;
	const char *BaseDesc;
	const char *SubDesc;
	const char *ProgDesc;
} PCI_CLASSCODETABLE, *PPCI_CLASSCODETABLE;

extern PCI_CLASSCODETABLE PciClassCodeTable[];
extern int PCI_CLASSCODETABLE_LEN;

extern const char *PciCommandFlags[];
extern int PCI_COMMANDFLAGS_LEN;

extern const char *PciStatusFlags[];
extern int PCI_STATUSFLAGS_LEN;

extern const char *PciDevSelFlags[];
extern int PCI_DEVSELFLAGS_LEN;

#include <lib/port.h>
#include <lib/lock.h>
#include <lib/klib.h>

static int pci_cache_ready;
static unsigned char pci_scanned_bus[256];

static void pci_scan_bus_raw(int bus);

static void pci_write_raw(unsigned device, int field, int size, unsigned value)
{
	port_write_dword(PCI_ADDRESS_PORT, pci_get_addr(device, field));
	if (size == 4) {
		port_write_dword(PCI_VALUE_PORT, value);
	} else if (size == 2) {
		port_write_word(PCI_VALUE_PORT + (field & 2), (uint16_t)value);
	} else if (size == 1) {
		port_write_byte(PCI_VALUE_PORT + (field & 3), (uint8_t)value);
	}
}

static unsigned pci_read_raw(unsigned device, int field, int size)
{
	port_write_dword(PCI_ADDRESS_PORT, pci_get_addr(device, field));

	if (size == 4) {
		unsigned t = port_read_dword(PCI_VALUE_PORT);
		return t;
	} else if (size == 2) {
		uint16_t t = port_read_word(PCI_VALUE_PORT + (field & 2));
		return t;
	} else if (size == 1) {
		uint8_t t = port_read_byte(PCI_VALUE_PORT + (field & 3));
		return t;
	}
	return 0xFFFF;
}

static spinlock_t pci_config_lock = SPINLOCK_INITIALIZER;

void pci_write_field(unsigned device, int field, int size, unsigned value)
{
	int irq;
	spinlock_lock(&pci_config_lock, &irq);
	pci_write_raw(device, field, size, value);
	spinlock_unlock(&pci_config_lock, irq);
}

unsigned pci_read_field(unsigned device, int field, int size)
{
	unsigned value;
	int irq;
	spinlock_lock(&pci_config_lock, &irq);
	value = pci_read_raw(device, field, size);
	spinlock_unlock(&pci_config_lock, irq);
	return value;
}

static void pci_probe_resources(unsigned device, pci_resource resources[7])
{
	unsigned i, bars, header, command, low, high, mask, mask_high, reg;
	uint64_t address, size_mask, length;
	int irq;

	memset(resources, 0, sizeof(pci_resource) * 7);
	spinlock_lock(&pci_config_lock, &irq);
	header = pci_read_raw(device, PCI_HEADER_TYPE, 1) & 0x7f;
	bars = header == 0 ? 6 : (header == 1 ? 2 : 0);
	command = pci_read_raw(device, PCI_COMMAND, 2);
	pci_write_raw(device, PCI_COMMAND, 2, command & ~3U);
	for (i = 0; i < bars; i++) {
		reg = PCI_BAR0 + i * 4;
		low = pci_read_raw(device, reg, 4);
		high = mask_high = 0;
		if (!(low & 1) && (low & 6) == 4 && i + 1 < bars)
			high = pci_read_raw(device, reg + 4, 4);
		pci_write_raw(device, reg, 4, ~0U);
		if (!(low & 1) && (low & 6) == 4 && i + 1 < bars)
			pci_write_raw(device, reg + 4, 4, ~0U);
		mask = pci_read_raw(device, reg, 4);
		if (!(low & 1) && (low & 6) == 4 && i + 1 < bars) {
			mask_high = pci_read_raw(device, reg + 4, 4);
			pci_write_raw(device, reg + 4, 4, high);
		}
		pci_write_raw(device, reg, 4, low);
		address = (uint64_t)high << 32 |
			  (low & ((low & 1) ? ~3U : ~15U));
		size_mask = mask & ((low & 1) ? ~3U : ~15U);
		if (!(low & 1) && (low & 6) == 4)
			size_mask |= (uint64_t)mask_high << 32;
		else
			size_mask |= (uint64_t)~0U << 32;
		length = ~size_mask + 1;
		if (address && length && mask) {
			resources[i].start = address;
			resources[i].size = length;
			resources[i].flags = (low & 1) ? 0x101 :
							 (0x200 | (low & 15));
		}
		if (!(low & 1) && (low & 6) == 4)
			i++;
	}
	if (header == 0 || header == 1) {
		reg = header == 0 ? 0x30 : 0x38;
		low = pci_read_raw(device, reg, 4);
		pci_write_raw(device, reg, 4, 0xfffff800U);
		mask = pci_read_raw(device, reg, 4) & 0xfffff800U;
		pci_write_raw(device, reg, 4, low);
		if ((low & 0xfffff800U) && mask) {
			resources[6].start = low & 0xfffff800U;
			resources[6].size = (uint32_t)(~mask + 1);
			resources[6].flags = 0x200;
		}
	}
	pci_write_raw(device, PCI_COMMAND, 2, command);
	spinlock_unlock(&pci_config_lock, irq);
}

uint16_t pci_find_type(unsigned dev)
{
	return (pci_read_field(dev, PCI_CLASS, 1) << 8) |
	       pci_read_field(dev, PCI_SUBCLASS, 1);
}

static void pci_cache_add(unsigned dev)
{
	device_t *device = zalloc(sizeof(*device));
	if (!device) {
		printk("pci: cannot record device %x\n", dev);
		return;
	}
	device->bus = DEVICE_BUS_PCI;
	device->address = dev;
	device->vendor_id = pci_read_field(dev, PCI_VENDOR_ID, 2);
	device->device_id = pci_read_field(dev, PCI_DEVICE_ID, 2);
	device->type = pci_find_type(dev);
	pci_probe_resources(dev, device->resources);
	device_register(device);
	device->sysfs_entry = pci_sysfs_create(device);
}

static void pci_scan_func_raw(int bus, int slot, int func)
{
	unsigned dev = pci_box_device(bus, slot, func);
	pci_cache_add(dev);
	if (pci_find_type(dev) == PCI_TYPE_BRIDGE) {
		pci_scan_bus_raw(pci_read_field(dev, PCI_SECONDARY_BUS, 1));
	}
}

static void pci_scan_slot_raw(int bus, int slot)
{
	int func;

	unsigned dev = pci_box_device(bus, slot, 0);
	if (pci_read_field(dev, PCI_VENDOR_ID, 2) == PCI_NONE) {
		return;
	}
	pci_scan_func_raw(bus, slot, 0);
	if (!(pci_read_field(dev, PCI_HEADER_TYPE, 1) & 0x80)) {
		return;
	}
	for (func = 1; func < 8; func++) {
		unsigned dev = pci_box_device(bus, slot, func);
		if (pci_read_field(dev, PCI_VENDOR_ID, 2) != PCI_NONE) {
			pci_scan_func_raw(bus, slot, func);
		}
	}
}

static void pci_scan_bus_raw(int bus)
{
	int slot;
	if (bus < 0 || bus > 255 || pci_scanned_bus[bus])
		return;
	pci_scanned_bus[bus] = 1;
	for (slot = 0; slot < 32; ++slot) {
		pci_scan_slot_raw(bus, slot);
	}
}

static void pci_scan_raw(void)
{
	int func;

	pci_scan_bus_raw(0);

	if (!(pci_read_field(0, PCI_HEADER_TYPE, 1) & 0x80)) {
		return;
	}

	for (func = 1; func < 8; ++func) {
		unsigned dev = pci_box_device(0, 0, func);
		if (pci_read_field(dev, PCI_VENDOR_ID, 2) != PCI_NONE) {
			pci_scan_bus_raw(func);
		}
	}
}

/* Called once on the boot CPU, before application processors start. */
void pci_scan(void)
{
	if (pci_cache_ready)
		return;
	pci_cache_ready = 1;
	pci_scan_raw();
}

/* Read-only inventory traversal; never scans or probes hardware. */
void pci_for_each(pci_func_t f, int type, void *extra)
{
	const device_t *device;
	if (!f)
		return;
	for (device = device_first(); device; device = device_next(device))
		if (device->bus == DEVICE_BUS_PCI &&
		    (type == PCI_SCAN_ALL || type == device->type))
			f(device->address, device->vendor_id, device->device_id,
			  extra);
}

void pci_get_resources(unsigned address, pci_resource resources[7])
{
	const device_t *device = device_find(DEVICE_BUS_PCI, address);
	memset(resources, 0, sizeof(pci_resource) * 7);
	if (device)
		memcpy(resources, device->resources, sizeof(device->resources));
}
#include <fs/sysfs.h>
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
	unsigned offset, width, shift;
} pci_fields[] = {
	{ "vendor", PCI_VENDOR_ID, 2 },	    { "device", PCI_DEVICE_ID, 2 },
	{ "class", PCI_REVISION_ID, 4, 8 }, { "revision", PCI_REVISION_ID, 1 },
	{ "subsystem_vendor", 0x2c, 2 },    { "subsystem_device", 0x2e, 2 },
};

static int sysfs_pci_register_show(void *data, unsigned tag, char *buf,
				   unsigned capacity)
{
	struct sysfs_pci *device = data;
	if (capacity < 1024)
		return -ENOSPC;
	unsigned value = pci_read_field(device->hardware->address,
					pci_fields[tag].offset,
					pci_fields[tag].width);
	return sprintf(buf, "0x%06x\n", value >> pci_fields[tag].shift);
}

static int sysfs_pci_boot_show(void *data, unsigned tag, char *buf,
			       unsigned capacity)
{
	struct sysfs_pci *device = data;
	if (capacity < 1024)
		return -ENOSPC;
	return sprintf(buf, "%u\n", device->hardware->boot_vga);
}

static int sysfs_pci_enable_show(void *data, unsigned tag, char *buf,
				 unsigned capacity)
{
	struct sysfs_pci *device = data;
	if (capacity < 1024)
		return -ENOSPC;
	return sprintf(
		buf, "%u\n",
		!!(pci_read_field(device->hardware->address, PCI_COMMAND, 2) &
		   3));
}

static int sysfs_pci_resources_show(void *data, unsigned tag, char *buf,
				    unsigned capacity)
{
	struct sysfs_pci *device = data;
	unsigned length = 0;
	if (capacity < 1024)
		return -ENOSPC;
	for (unsigned i = 0; i < 7; i++) {
		const pci_resource *resource = &device->hardware->resources[i];
		uint64_t end = resource->size ?
				       resource->start + resource->size - 1 :
				       0;
		length += sprintf(buf + length, "%08x%08x %08x%08x %08x%08x\n",
				  (unsigned)(resource->start >> 32),
				  (unsigned)resource->start,
				  (unsigned)(end >> 32), (unsigned)end, 0U,
				  resource->flags);
	}
	return length;
}

static int sysfs_pci_uevent_show(void *data, unsigned tag, char *buf,
				 unsigned capacity)
{
	struct sysfs_pci *device = data;
	if (capacity < 1024)
		return -ENOSPC;
	return sprintf(
		buf, "PCI_SLOT_NAME=%s\n%s%s%s", vfs_entry_name(device->node),
		device->hardware->driver ? "DRIVER=" : "",
		device->hardware->driver ? device->hardware->driver->name : "",
		device->hardware->driver ? "\n" : "");
}

static int sysfs_pci_config_show(void *data, unsigned tag, char *buf,
				 unsigned capacity)
{
	return capacity < 1024 ? -ENOSPC : 256;
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

static ssize_t sysfs_pci_config_write(void *data, unsigned tag, const void *buf,
				      size_t size, loff_t *pos)
{
	struct sysfs_pci *device = data;
	if ((uint64_t)*pos >= 256)
		return -EFBIG;
	if (size > 256 - *pos)
		size = 256 - *pos;
	for (unsigned i = 0; i < size; i++)
		pci_write_field(device->hardware->address, *pos + i, 1,
				((const unsigned char *)buf)[i]);
	*pos += size;
	return size;
}

static ssize_t sysfs_pci_enable_write(void *data, unsigned tag, const void *buf,
				      size_t size, loff_t *pos)
{
	struct sysfs_pci *device = data;
	if (!size)
		return 0;
	if (*(const char *)buf != '0' && *(const char *)buf != '1')
		return -EINVAL;
	unsigned command =
		pci_read_field(device->hardware->address, PCI_COMMAND, 2);
	command = *(const char *)buf == '1' ? command | 3 : command & ~3U;
	pci_write_field(device->hardware->address, PCI_COMMAND, 2, command);
	*pos += size;
	return size;
}

static const vfs_entry_attribute_ops pci_text_ops = {
	.show = sysfs_pci_register_show
};
static const vfs_entry_attribute_ops pci_boot_ops = {
	.show = sysfs_pci_boot_show
};
static const vfs_entry_attribute_ops pci_resources_ops = {
	.show = sysfs_pci_resources_show
};
static const vfs_entry_attribute_ops pci_uevent_ops = {
	.show = sysfs_pci_uevent_show
};
static const vfs_entry_attribute_ops pci_enable_ops = {
	.show = sysfs_pci_enable_show,
	.write = sysfs_pci_enable_write,
};
static const vfs_entry_attribute_ops pci_config_ops = {
	.show = sysfs_pci_config_show,
	.read = sysfs_pci_read,
	.write = sysfs_pci_config_write,
};

static vfs_entry_node *pci_sysfs_create(const device_t *hardware)
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
	device->node = vfs_entry_directory(devices, name);
	if (!device->node)
		return NULL;
	for (i = 0; i < sizeof(pci_fields) / sizeof(pci_fields[0]); i++)
		vfs_entry_attribute(device->node, pci_fields[i].name, 0444,
				    &pci_text_ops, device, i);
	vfs_entry_attribute(device->node, "boot_vga", 0444, &pci_boot_ops,
			    device, PCI_ATTR_BOOT);
	vfs_entry_attribute(device->node, "enable", 0644, &pci_enable_ops,
			    device, PCI_ATTR_ENABLE);
	vfs_entry_attribute(device->node, "resource", 0444, &pci_resources_ops,
			    device, PCI_ATTR_RESOURCES);
	vfs_entry_attribute(device->node, "config", 0644, &pci_config_ops,
			    device, PCI_ATTR_CONFIG);
	vfs_entry_attribute(device->node, "uevent", 0444, &pci_uevent_ops,
			    device, PCI_ATTR_UEVENT);
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
	vfs_entry_link(pci_sysfs_devices(), vfs_entry_name(device->node),
		       device->node);
	return device->node;
}

static vfs_entry_node *pci_sysfs_bus(void)
{
	vfs_entry_tree *tree = device_sysfs_entries();
	if (!tree)
		return NULL;
	return driver_bus_entry(DEVICE_BUS_PCI);
}

static vfs_entry_node *pci_sysfs_devices(void)
{
	return vfs_entry_directory(pci_sysfs_bus(), "devices");
}

static vfs_entry_node *pci_sysfs_hardware(void)
{
	vfs_entry_tree *tree = device_sysfs_entries();
	if (!tree)
		return NULL;
	return vfs_entry_directory(vfs_entry_directory(vfs_entry_root(tree),
						       "devices"),
				   "pci0000:00");
}
