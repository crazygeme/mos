#ifndef MOS_DEVICE_DEVICE_H
#define MOS_DEVICE_DEVICE_H

#include <device/pci.h>
#include <device/bus.h>

struct driver_t;

typedef struct device_t {
	device_bus_t bus;
	/* PCI BDF or PS/2 port number, scoped by bus. */
	uint32_t address;
	uint16_t vendor_id, device_id, type;
	unsigned boot_vga;
	pci_resource resources[7];
	struct driver_t *selected_driver;
	struct driver_t *driver;
	int probe_done, probe_error;
	struct device_t *next;
} device_t;

/* Boot-thread registration; records remain valid for the kernel lifetime. */
void device_register(device_t *device);
const device_t *device_first(void);
void device_probe(device_t *device);

#endif
