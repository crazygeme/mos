#include <driver/driver.h>
#include <lib/lock.h>

static list_entry devices = { &devices, &devices };
static unsigned boot_vga;
static spinlock_t inventory_lock = SPINLOCK_INITIALIZER;
#define DEVICE_BUS_COUNT (DEVICE_BUS_PLATFORM + 1)
static list_entry bus_devices[DEVICE_BUS_COUNT];
static struct rb_root device_addresses = _RBTREE_ROOT_INIT;

static int device_compare(device_bus_t bus, uint32_t address,
			  const device_t *device)
{
	if (bus != device->bus)
		return bus < device->bus ? -1 : 1;
	return address < device->address ? -1 : address != device->address;
}

const device_t *device_find(device_bus_t bus, uint32_t address)
{
	int irq;
	spinlock_lock(&inventory_lock, &irq);
	struct rb_node *node = device_addresses.rb_node;
	while (node) {
		device_t *device = rb_entry(node, device_t, address_node);
		int order = device_compare(bus, address, device);
		if (!order) {
			spinlock_unlock(&inventory_lock, irq);
			return device;
		}
		node = order < 0 ? node->rb_left : node->rb_right;
	}
	spinlock_unlock(&inventory_lock, irq);
	return NULL;
}

const device_t *device_first(void)
{
	return list_is_empty(&devices) ?
		       NULL :
		       container_of(devices.next, device_t, list);
}

const device_t *device_next(const device_t *device)
{
	return device->list.next == &devices ?
		       NULL :
		       container_of(device->list.next, device_t, list);
}

void device_register(device_t *device)
{
	int irq;
	spinlock_lock(&inventory_lock, &irq);
	struct rb_node **link = &device_addresses.rb_node, *parent = NULL;
	while (*link) {
		device_t *existing = rb_entry(*link, device_t, address_node);
		int order =
			device_compare(device->bus, device->address, existing);
		if (!order) {
			spinlock_unlock(&inventory_lock, irq);
			return;
		}
		parent = *link;
		link = order < 0 ? &parent->rb_left : &parent->rb_right;
	}
	rb_init_node(&device->address_node);
	rb_link_node(&device->address_node, parent, link);
	rb_insert_color(&device->address_node, &device_addresses);
	list_insert_tail(&devices, &device->list);
	list_entry *bus = &bus_devices[device->bus];
	if (!bus->next)
		list_init(bus);
	list_insert_tail(bus, &device->bus_list);
	if (device->bus == DEVICE_BUS_PCI && !boot_vga &&
	    device->type == 0x0300)
		device->boot_vga = boot_vga = 1;
	spinlock_unlock(&inventory_lock, irq);
}

/* Dynamic bus providers serialize removal with their own lifecycle locks. */
void device_unregister(device_t *device)
{
	int irq;
	spinlock_lock(&inventory_lock, &irq);
	rb_erase(&device->address_node, &device_addresses);
	list_remove_entry(&device->list);
	list_remove_entry(&device->bus_list);
	spinlock_unlock(&inventory_lock, irq);
}
const device_t *device_bus_first(device_bus_t bus)
{
	list_entry *head = &bus_devices[bus];
	return !head->next || list_is_empty(head) ?
		       NULL :
		       container_of(head->next, device_t, bus_list);
}
const device_t *device_bus_next(const device_t *device)
{
	list_entry *head = &bus_devices[device->bus];
	return device->bus_list.next == head ?
		       NULL :
		       container_of(device->bus_list.next, device_t, bus_list);
}
