#ifndef _DRIVER_DRIVER_H_
#define _DRIVER_DRIVER_H_

#include <stdint.h>
#include <device/bus.h>
#include <device/pci.h>

struct device_t;

typedef enum {
	DRIVER_TYPE_HID = 0,
	DRIVER_TYPE_VIDEO,
	DRIVER_TYPE_AUDIO,
	DRIVER_TYPE_NET,
	DRIVER_TYPE_STORAGE,
	DRIVER_TYPE_TIMER,
	DRIVER_TYPE_SERIAL,
	DRIVER_TYPE_OTHER,
} driver_type_t;

typedef struct driver_t {
	const char *name;
	driver_type_t type;
	device_bus_t bus;
	const pci_device_id *pci_ids;
	unsigned pci_id_count;
	const void *ops;
	/* Probe during bus discovery instead of the level-2 device pass.
	 * PCI early probes run before scheduling and IRQ setup. */
	unsigned early;
	void (*console_init)(uint32_t device);
	int (*probe_pci)(uint32_t device, uint16_t vendor_id,
			 uint16_t device_id, const pci_device_id *id);
	unsigned ps2_port;
	int (*probe_ps2)(unsigned port);
	void (*receive_ps2)(unsigned char byte);
	struct driver_t *next;
} driver_t;

void driver_register(driver_t *driver);
void drivers_init(void);
driver_t *driver_first(void);
/* Returns the unique matching driver; conflicting matches remain unbound. */
driver_t *driver_select(const struct device_t *device);
const pci_device_id *driver_match_pci(const driver_t *driver,
				      const struct device_t *device);

/* Registration has no ordering semantics. */
#define DRIVER_REGISTER(driver)                  \
	static driver_t *const __driver_##driver \
		__attribute__((used, section(".driver"))) = &(driver)

#endif
