#ifndef _HW_DRIVER_H_
#define _HW_DRIVER_H_

#include <stdint.h>

typedef enum {
	HW_BUS_PCI = 0,
	HW_BUS_USB,
} hw_bus_t;

typedef enum {
	HW_TYPE_HID = 0,
	HW_TYPE_VIDEO,
	HW_TYPE_AUDIO,
	HW_TYPE_NET,
	HW_TYPE_STORAGE,
	HW_TYPE_TIMER,
	HW_TYPE_SERIAL,
	HW_TYPE_OTHER,
} hw_type_t;

typedef struct {
	uint16_t vendor_id;
	uint16_t device_id;
} hw_pci_id;

typedef struct hw_driver {
	const char *name;
	hw_type_t type;
	hw_bus_t bus;
	const hw_pci_id *pci_ids;
	unsigned pci_id_count;
	const void *ops;
	int (*probe_pci)(uint32_t device, uint16_t vendor_id,
			 uint16_t device_id, const hw_pci_id *id);
	struct hw_driver *next;
} hw_driver;

void hw_driver_register(hw_driver *driver);
int hw_probe_pci(uint32_t device, uint16_t vendor_id, uint16_t device_id);
void hw_pci_probe_all(void);

#endif
