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

void pci_write_field(unsigned device, int field, int size, unsigned value)
{
	port_write_dword(PCI_ADDRESS_PORT, pci_get_addr(device, field));
	port_write_dword(PCI_VALUE_PORT, value);
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

void pci_scan_hit(pci_func_t f, unsigned dev, void *extra)
{
	int dev_vend = (int)pci_read_field(dev, PCI_VENDOR_ID, 2);
	int dev_dvid = (int)pci_read_field(dev, PCI_DEVICE_ID, 2);

	f(dev, dev_vend, dev_dvid, extra);
}

void pci_scan_func(pci_func_t f, int type, int bus, int slot, int func,
		   void *extra)
{
	unsigned dev = pci_box_device(bus, slot, func);
	if (type == -1 || type == pci_find_type(dev)) {
		pci_scan_hit(f, dev, extra);
	}
	if (pci_find_type(dev) == PCI_TYPE_BRIDGE) {
		pci_scan_bus(f, type, pci_read_field(dev, PCI_SECONDARY_BUS, 1),
			     extra);
	}
}

void pci_scan_slot(pci_func_t f, int type, int bus, int slot, void *extra)
{
	int func;

	unsigned dev = pci_box_device(bus, slot, 0);
	if (pci_read_field(dev, PCI_VENDOR_ID, 2) == PCI_NONE) {
		return;
	}
	pci_scan_func(f, type, bus, slot, 0, extra);
	if (!pci_read_field(dev, PCI_HEADER_TYPE, 1)) {
		return;
	}
	for (func = 1; func < 8; func++) {
		unsigned dev = pci_box_device(bus, slot, func);
		if (pci_read_field(dev, PCI_VENDOR_ID, 2) != PCI_NONE) {
			pci_scan_func(f, type, bus, slot, func, extra);
		}
	}
}

void pci_scan_bus(pci_func_t f, int type, int bus, void *extra)
{
	int slot;
	for (slot = 0; slot < 32; ++slot) {
		pci_scan_slot(f, type, bus, slot, extra);
	}
}

void pci_scan(pci_func_t f, int type, void *extra)
{
	int func;

	pci_scan_bus(f, type, 0, extra);

	if (!pci_read_field(0, PCI_HEADER_TYPE, 1)) {
		return;
	}

	for (func = 1; func < 8; ++func) {
		unsigned dev = pci_box_device(0, 0, func);
		if (pci_read_field(dev, PCI_VENDOR_ID, 2) != PCI_NONE) {
			pci_scan_bus(f, type, func, extra);
		} else {
			break;
		}
	}
}
