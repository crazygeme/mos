#include <driver/driver.h>

static list_entry devices = { &devices, &devices };
static unsigned boot_vga;
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
	struct rb_node *node = device_addresses.rb_node;
	while (node) {
		device_t *device = rb_entry(node, device_t, address_node);
		int order = device_compare(bus, address, device);
		if (!order)
			return device;
		node = order < 0 ? node->rb_left : node->rb_right;
	}
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
	struct rb_node **link = &device_addresses.rb_node, *parent = NULL;
	while (*link) {
		device_t *existing = rb_entry(*link, device_t, address_node);
		int order =
			device_compare(device->bus, device->address, existing);
		if (!order)
			return;
		parent = *link;
		link = order < 0 ? &parent->rb_left : &parent->rb_right;
	}
	rb_init_node(&device->address_node);
	rb_link_node(&device->address_node, parent, link);
	rb_insert_color(&device->address_node, &device_addresses);
	list_insert_tail(&devices, &device->list);
	if (device->bus == DEVICE_BUS_PCI && !boot_vga &&
	    device->type == 0x0300)
		device->boot_vga = boot_vga = 1;
}
