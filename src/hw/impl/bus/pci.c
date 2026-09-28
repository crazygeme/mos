/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2011-2014 Kevin Lange
 *
 * ToAruOS PCI Initialization
 */

#include <int/int.h>
#include <hw/pci.h>
#include <hw/pci_list.h>
#include <lib/port.h>
#include <lib/lock.h>
#include <lib/klib.h>

#define PCI_MAX_DEVICES 128

typedef struct {
	uint32_t device;
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t type;
	pci_resource resources[7];
} pci_cached_device;

static pci_cached_device pci_devices[PCI_MAX_DEVICES];
static unsigned pci_device_count;
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

static spinlock_t pci_config_lock = { .inited = 1 };

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
	if (pci_device_count >= PCI_MAX_DEVICES)
		return;

	pci_devices[pci_device_count].device = dev;
	pci_devices[pci_device_count].vendor_id =
		(uint16_t)pci_read_field(dev, PCI_VENDOR_ID, 2);
	pci_devices[pci_device_count].device_id =
		(uint16_t)pci_read_field(dev, PCI_DEVICE_ID, 2);
	pci_devices[pci_device_count].type = pci_find_type(dev);
	pci_probe_resources(dev, pci_devices[pci_device_count].resources);
	pci_device_count++;
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
	if (!pci_read_field(dev, PCI_HEADER_TYPE, 1)) {
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

	if (!pci_read_field(0, PCI_HEADER_TYPE, 1)) {
		return;
	}

	for (func = 1; func < 8; ++func) {
		unsigned dev = pci_box_device(0, 0, func);
		if (pci_read_field(dev, PCI_VENDOR_ID, 2) != PCI_NONE) {
			pci_scan_bus_raw(func);
		} else {
			break;
		}
	}
}

static void pci_scan_cache(void)
{
	if (pci_cache_ready)
		return;

	pci_device_count = 0;
	pci_scan_raw();
	pci_cache_ready = 1;
}

void pci_scan_bus(pci_func_t f, int type, int bus, void *extra)
{
	unsigned i;

	if (!f)
		return;

	pci_scan_cache();
	for (i = 0; i < pci_device_count; i++) {
		if (pci_extract_bus(pci_devices[i].device) != bus)
			continue;
		if (type == PCI_SCAN_ALL || type == pci_devices[i].type)
			f(pci_devices[i].device, pci_devices[i].vendor_id,
			  pci_devices[i].device_id, extra);
	}
}

void pci_scan(pci_func_t f, int type, void *extra)
{
	unsigned i;

	if (!f)
		return;

	pci_scan_cache();
	for (i = 0; i < pci_device_count; i++) {
		if (type == PCI_SCAN_ALL || type == pci_devices[i].type)
			f(pci_devices[i].device, pci_devices[i].vendor_id,
			  pci_devices[i].device_id, extra);
	}
}

void pci_get_resources(unsigned device, pci_resource resources[7])
{
	unsigned i;
	pci_scan_cache();
	memset(resources, 0, sizeof(pci_resource) * 7);
	for (i = 0; i < pci_device_count; i++) {
		if (pci_devices[i].device == device) {
			memcpy(resources, pci_devices[i].resources,
			       sizeof(pci_resource) * 7);
			return;
		}
	}
}
