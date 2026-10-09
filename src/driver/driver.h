#ifndef _DRIVER_DRIVER_H_
#define _DRIVER_DRIVER_H_

#include <stdint.h>
#include <lib/list.h>
#include <mm/mm.h>
#include <lib/rbtree.h>

typedef enum {
	DEVICE_BUS_PCI = 0,
	DEVICE_BUS_USB,
	DEVICE_BUS_PS2,
	DEVICE_BUS_VIRTUAL,
	DEVICE_BUS_PLATFORM,
} device_bus_t;

#define PCI_VENDOR_ID 0x00 // 2
#define PCI_DEVICE_ID 0x02 // 2
#define PCI_COMMAND 0x04 // 2
#define PCI_STATUS 0x06 // 2
#define PCI_REVISION_ID 0x08 // 1

#define PCI_PROG_IF 0x09 // 1
#define PCI_SUBCLASS 0x0a // 1
#define PCI_CLASS 0x0b // 1
#define PCI_CACHE_LINE_SIZE 0x0c // 1
#define PCI_LATENCY_TIMER 0x0d // 1
#define PCI_HEADER_TYPE 0x0e // 1
#define PCI_BIST 0x0f // 1
#define PCI_BAR0 0x10 // 4
#define PCI_BAR1 0x14 // 4
#define PCI_BAR2 0x18 // 4
#define PCI_BAR3 0x1C // 4
#define PCI_BAR4 0x20 // 4
#define PCI_BAR5 0x24 // 4

#define PCI_INTERRUPT_LINE 0x3C // 1

#define PCI_SECONDARY_BUS 0x19 // 1

#define PCI_HEADER_TYPE_DEVICE 0
#define PCI_HEADER_TYPE_BRIDGE 1
#define PCI_HEADER_TYPE_CARDBUS 2

#define PCI_TYPE_BRIDGE 0x0604
#define PCI_TYPE_SATA 0x0106

#define PCI_ADDRESS_PORT 0xCF8
#define PCI_VALUE_PORT 0xCFC

#define PCI_NONE 0xFFFF

#define PCI_SCAN_ALL (-1)
#define PCI_ANY_ID 0xffff

typedef struct {
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t class_code, class_mask;
} pci_device_id;

typedef void (*pci_func_t)(uint32_t device, uint16_t vendor_id,
			   uint16_t device_id, void *extra);

static inline int pci_extract_bus(uint32_t device)
{
	return (uint8_t)((device >> 16));
}
static inline int pci_extract_slot(uint32_t device)
{
	return (uint8_t)((device >> 8));
}
static inline int pci_extract_func(uint32_t device)
{
	return (uint8_t)(device);
}

static inline uint32_t pci_get_addr(uint32_t device, int field)
{
	return 0x80000000 | (pci_extract_bus(device) << 16) |
	       (pci_extract_slot(device) << 11) |
	       (pci_extract_func(device) << 8) | ((field) & 0xFC);
}

static inline uint32_t pci_box_device(int bus, int slot, int func)
{
	return (uint32_t)((bus << 16) | (slot << 8) | func);
}

void pci_scan(void);
/* Iterate the boot inventory without discovery or driver side effects. */
void pci_for_each(pci_func_t f, int type, void *extra);

unsigned pci_read_field(unsigned device, int field, int size);
void pci_write_field(unsigned device, int field, int size, unsigned value);

typedef struct {
	uint64_t start, size;
	unsigned flags;
} pci_resource;

void pci_get_resources(unsigned device, pci_resource resources[7]);

enum virtual_device_id {
	VDEV_TTY,
	VDEV_PTY,
	VDEV_PTMX,
	VDEV_LOOP,
	VDEV_MEM,
	VDEV_NULL,
	VDEV_ZERO,
	VDEV_RANDOM,
	VDEV_KMSG,
	VDEV_INITCTL,
	VDEV_FD,
	VDEV_COUNT,
};
void virtual_scan(void);
void platform_scan(void);

struct vfs_entry_node;
struct driver_t;

typedef struct device_t {
	device_bus_t bus;
	/* PCI BDF or PS/2 port number, scoped by bus. */
	uint32_t address;
	uint16_t vendor_id, device_id, type;
	unsigned boot_vga;
	pci_resource resources[7];
	struct vfs_entry_node *sysfs_entry;
	struct driver_t *selected_driver;
	const pci_device_id *matched_pci_id;
	struct driver_t *driver;
	int match_done, probe_done, probe_error;
	unsigned boot_logged;
	list_entry list;
	struct rb_node address_node;
} device_t;

/* Inventory only: registration never matches or probes drivers.
 * Boot-thread records remain valid for the kernel lifetime. */
void device_register(device_t *device);
const device_t *device_first(void);
const device_t *device_next(const device_t *device);
const device_t *device_find(device_bus_t bus, uint32_t address);

#define PS2_PORT_KEYBOARD 0
#define PS2_PORT_AUX 1
#define PS2_REPLY_SPINS 500000

void ps2_scan(void);
const device_t *ps2_device(unsigned port);
void ps2_drain_input(void);
/* The protocol driver serializes command transactions on its port. */
void ps2_command_begin(unsigned port);
void ps2_command_end(unsigned port);
int ps2_write_byte(unsigned port, unsigned char data);
int ps2_read_reply(unsigned port, unsigned char *data, int spins);

struct device_t;

typedef struct driver_t {
	const char *name;
	struct vfs_entry_node *sysfs_entry;
	device_bus_t bus;
	const pci_device_id *pci_ids;
	unsigned pci_id_count;
	/* Probe after bus discovery instead of the level-2 driver pass.
	 * PCI early probes run before scheduling and IRQ setup. */
	unsigned early;
	void (*console_init)(uint32_t device);
	int (*probe_pci)(uint32_t device, uint16_t vendor_id,
			 uint16_t device_id, const pci_device_id *id);
	uint32_t platform_address;
	int (*probe_platform)(uint32_t address);
	/* Start worker tasks only after bootstrap process IDs are reserved. */
	void (*start)(void);
	unsigned started;
	unsigned virtual_id;
	int (*probe_virtual)(void);
	unsigned ps2_port;
	int (*probe_ps2)(unsigned port);
	void (*receive_ps2)(unsigned char byte);
	list_entry list;
	unsigned registered;
} driver_t;

struct vfs_entry_node *driver_device_entry(device_bus_t bus, uint32_t address);
struct vfs_entry_node *driver_bus_entry(device_bus_t bus);
void driver_register(driver_t *driver);
void drivers_init(void);
/* Called after each boot bus scan; ordinary probes wait until init level 2. */
void drivers_bind_early(void);
void drivers_start_workers(void);
/* Probe a selected device once and record the successful binding or error. */
void driver_bind(struct device_t *device);
driver_t *driver_first(void);
driver_t *driver_next(const driver_t *driver);
/* Returns the unique matching driver; conflicting matches remain unbound. */
driver_t *driver_select(struct device_t *device);
int driver_probe(struct device_t *device);
const pci_device_id *driver_match_pci(const driver_t *driver,
				      const struct device_t *device);

/* Registration has no ordering semantics. */
#define DRIVER_REGISTER(driver)                  \
	static driver_t *const __driver_##driver \
		__attribute__((used, section(".driver"))) = &(driver)

#endif
