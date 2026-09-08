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

#define PCI_MAX_DEVICES 128

typedef struct {
	uint32_t device;
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t type;
} pci_cached_device;

static pci_cached_device pci_devices[PCI_MAX_DEVICES];
static unsigned pci_device_count;
static int pci_cache_ready;

static void pci_scan_bus_raw(int bus);

void pci_write_field(unsigned device, int field, int size, unsigned value)
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

unsigned pci_read_field(unsigned device, int field, int size)
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
