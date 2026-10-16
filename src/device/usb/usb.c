/* USB configuration discovery and interface inventory for host controllers. */
#include "usb.h"
#include <fs/sysfs.h>
#include <lib/klib.h>
#include <errno.h>
#include <macro.h>

static unsigned next_address;
static mutex_t lifecycle_lock;
static void usb_bus_init(void)
{
	mutex_init(&lifecycle_lock);
}
KERNEL_INIT(1, usb_bus_init);

int usb_control(struct usb_device *u, unsigned type, unsigned request,
		unsigned value, unsigned index, void *data, unsigned length)
{
	return u->ops->control(u, type, request, value, index, data, length);
}
int usb_transfer(struct usb_device *u, struct usb_endpoint *ep, void *data,
		 unsigned length)
{
	return u->ops->transfer(u, ep, data, length);
}
int usb_interrupt_start(struct usb_device *u, struct usb_endpoint *ep,
			void (*receive)(struct usb_endpoint *,
					const unsigned char *, unsigned))
{
	return u->ops->interrupt_start(u, ep, receive);
}
int usb_clear_halt(struct usb_device *u, struct usb_endpoint *ep)
{
	return u->ops->clear_halt(u, ep);
}
static int descriptor_read(struct usb_device *u, unsigned type, void *buffer,
			   unsigned size)
{
	return usb_control(u, USB_DIR_IN | USB_RECIP_DEVICE,
			   USB_REQ_GET_DESCRIPTOR,
			   USB_DESCRIPTOR_VALUE(type, 0), 0, buffer, size);
}
static void select_alternate(struct usb_interface *interface,
			     struct usb_alternate *alternate)
{
	interface->active = alternate;
	interface->endpoint_count = alternate->endpoint_count;
	for (unsigned i = 0; i < alternate->endpoint_count; i++)
		interface->endpoints[i] = &alternate->endpoints[i];
}
static int collect_endpoints(struct usb_device *u)
{
	u->endpoint_count = 0;
	for (unsigned i = 0; i < u->interface_count; i++) {
		struct usb_interface *interface = &u->interfaces[i];
		for (unsigned j = 0; j < interface->endpoint_count; j++) {
			struct usb_endpoint *ep = interface->endpoints[j];
			if (u->endpoint_count == USB_MAX_ENDPOINTS)
				return -ENOSPC;
			for (unsigned k = 0; k < u->endpoint_count; k++)
				if (u->endpoints[k]->address == ep->address)
					return -EINVAL;
			u->endpoints[u->endpoint_count++] = ep;
		}
	}
	return 0;
}
static int parse_interfaces(struct usb_device *u, unsigned char *config,
			    unsigned length)
{
	struct usb_alternate *alternate = NULL;
	struct usb_endpoint *last = NULL;
	unsigned expected = 0, alternate_count = 0;
	for (unsigned offset = sizeof(struct usb_configuration_descriptor);
	     offset < length;) {
		struct usb_descriptor_header *header =
			(void *)(config + offset);
		if (length - offset < sizeof(*header) ||
		    header->length < sizeof(*header) ||
		    header->length > length - offset)
			return -EINVAL;
		if (header->type == USB_DESC_INTERFACE) {
			struct usb_interface_descriptor *d = (void *)header;
			if (header->length < sizeof(*d) ||
			    (alternate &&
			     alternate->endpoint_count != expected) ||
			    ++alternate_count > 128)
				return -EINVAL;
			if (alternate)
				alternate->descriptor_length =
					config + offset -
					alternate->descriptors;
			struct usb_interface *interface = NULL;
			for (unsigned i = 0; i < u->interface_count; i++)
				if (u->interfaces[i].number == d->number)
					interface = &u->interfaces[i];
			if (!interface) {
				if (u->interface_count == USB_MAX_INTERFACES)
					return -ENOSPC;
				interface =
					&u->interfaces[u->interface_count++];
				interface->usb = u;
				interface->number = d->number;
				interface->device.type = d->interface_class;
				interface->subclass = d->subclass;
				interface->protocol = d->protocol;
			}
			for (struct usb_alternate *p = interface->alternates; p;
			     p = p->next)
				if (p->number == d->alternate)
					return -EINVAL;
			alternate = zalloc(sizeof(*alternate));
			if (!alternate)
				return -ENOMEM;
			alternate->number = d->alternate;
			alternate->descriptors = config + offset;
			alternate->next = interface->alternates;
			interface->alternates = alternate;
			expected = d->endpoints;
			last = NULL;
		} else if (header->type == USB_DESC_ENDPOINT) {
			struct usb_endpoint_descriptor *d = (void *)header;
			if (!alternate || header->length < sizeof(*d) ||
			    alternate->endpoint_count == USB_MAX_ENDPOINTS)
				return -EINVAL;
			unsigned transactions = (d->max_packet >> 11) & 3;
			unsigned type = d->attributes & USB_ENDPOINT_TYPE_MASK;
			if ((d->address & USB_ENDPOINT_RESERVED_MASK) ||
			    !(d->address & USB_ENDPOINT_NUMBER_MASK) ||
			    !d->max_packet || (d->max_packet & 0xe000) ||
			    transactions == 3 ||
			    (transactions && (u->speed != USB_SPEED_HIGH ||
					      (type != USB_XFER_INTERRUPT &&
					       type != USB_XFER_ISOCHRONOUS))))
				return -EINVAL;
			for (unsigned i = 0; i < alternate->endpoint_count; i++)
				if (alternate->endpoints[i].address ==
				    d->address)
					return -EINVAL;
			last = &alternate
					->endpoints[alternate->endpoint_count++];
			last->address = d->address;
			last->attributes = d->attributes;
			last->max_packet = d->max_packet &
					   USB_ENDPOINT_PACKET_MASK;
			last->interval = d->interval;
			last->burst = transactions;
		} else if (header->type == USB_DESC_SS_ENDPOINT_COMPANION) {
			struct usb_ss_endpoint_descriptor *d = (void *)header;
			if (!last || u->speed < USB_SPEED_SUPER ||
			    header->length < sizeof(*d) || d->max_burst > 15)
				return -EINVAL;
			last->burst = d->max_burst;
			last->mult =
				(last->attributes & USB_ENDPOINT_TYPE_MASK) ==
						USB_XFER_ISOCHRONOUS ?
					d->attributes & 3 :
					0;
			if (last->mult == 3)
				return -EINVAL;
			last->bytes_interval = d->bytes_per_interval;
		}
		offset += header->length;
	}
	if (!alternate || alternate->endpoint_count != expected ||
	    u->interface_count !=
		    ((struct usb_configuration_descriptor *)config)->interfaces)
		return -EINVAL;
	alternate->descriptor_length = config + length - alternate->descriptors;
	for (unsigned i = 0; i < u->interface_count; i++) {
		struct usb_interface *interface = &u->interfaces[i];
		struct usb_alternate *p = interface->alternates;
		while (p && p->number != USB_DEFAULT_ALTERNATE)
			p = p->next;
		if (!p)
			return -EINVAL;
		select_alternate(interface, p);
	}
	return collect_endpoints(u);
}
void usb_device_cleanup(struct usb_device *u)
{
	for (unsigned i = 0; i < u->interface_count; i++) {
		struct usb_alternate *p = u->interfaces[i].alternates;
		while (p) {
			struct usb_alternate *next = p->next;
			free(p);
			p = next;
		}
	}
	free(u->configuration_data);
}
int usb_iso_submit(struct usb_device *u, struct usb_endpoint *ep, void *data,
		   struct usb_iso_packet *packets, unsigned count,
		   usb_iso_complete complete, void *context)
{
	if (!u->ops->iso_submit)
		return -EOPNOTSUPP;
	return u->ops->iso_submit(u, ep, data, packets, count, complete,
				  context);
}
int usb_set_interface(struct usb_interface *interface, unsigned number)
{
	struct usb_device *u = interface->usb;
	struct usb_alternate *target = interface->alternates;
	while (target && target->number != number)
		target = target->next;
	if (!target || !u->ops->interface_stop)
		return -EINVAL;
	mutex_lock(&u->lock);
	struct usb_alternate *old = interface->active;
	if (target == old) {
		mutex_unlock(&u->lock);
		return 0;
	}
	select_alternate(interface, target);
	int ret = collect_endpoints(u);
	select_alternate(interface, old);
	collect_endpoints(u);
	if (ret < 0)
		goto out;
	ret = u->ops->interface_stop(u, interface);
	if (ret < 0)
		goto out;
	ret = usb_control(u, USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE, number,
			  interface->number, NULL, 0);
	if (ret >= 0) {
		select_alternate(interface, target);
		collect_endpoints(u);
		ret = u->ops->configure(u);
	}
	if (ret < 0) {
		u->ops->interface_stop(u, interface);
		select_alternate(interface, old);
		collect_endpoints(u);
		if (usb_control(u, USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE,
				old->number, interface->number, NULL, 0) < 0 ||
		    u->ops->configure(u) < 0)
			u->online = 0;
	}
out:
	mutex_unlock(&u->lock);
	return ret;
}
static void physical_register(struct usb_device *u)
{
	if (u->sysfs_entry)
		return;
	vfs_entry_node *root = vfs_entry_root(device_sysfs_entries());
	vfs_entry_node *parent = u->parent ?
					 u->parent->sysfs_entry :
					 vfs_entry_directory(root, "devices");
	char name[48], text[96];
	sprintf(name, "%u-%s", u->busnum, u->path);
	u->sysfs_entry = vfs_entry_directory(parent, name);
	sprintf(u->sysfs_path, "%s/%s",
		u->parent ? u->parent->sysfs_path : "/devices", name);
	vfs_entry_link(vfs_entry_directory(driver_bus_entry(DEVICE_BUS_USB),
					   "devices"),
		       name, u->sysfs_entry);
	vfs_entry_link(u->sysfs_entry, "subsystem",
		       driver_bus_entry(DEVICE_BUS_USB));
	sprintf(text, "DEVTYPE=usb_device\nPRODUCT=%x/%x/0\n", u->vendor,
		u->product);
	vfs_entry_text(u->sysfs_entry, "uevent", text);
	sprintf(text, "%04x\n", u->vendor);
	vfs_entry_text(u->sysfs_entry, "idVendor", text);
	sprintf(text, "%04x\n", u->product);
	vfs_entry_text(u->sysfs_entry, "idProduct", text);
	sprintf(text, "%u\n", u->busnum);
	vfs_entry_text(u->sysfs_entry, "busnum", text);
	sprintf(text, "%u\n", u->address);
	vfs_entry_text(u->sysfs_entry, "devnum", text);
	vfs_entry_text(u->sysfs_entry, "devpath", u->path);
}
static void interface_register(struct usb_interface *interface)
{
	struct usb_device *u = interface->usb;
	device_t *d = &interface->device;
	d->bus = DEVICE_BUS_USB;
	d->address = ++next_address;
	d->vendor_id = u->vendor;
	d->device_id = u->product;
	char name[40], text[40];
	sprintf(name, "usb%u-%u:%u", d->address, u->port, interface->number);
	physical_register(u);
	d->sysfs_entry = vfs_entry_directory(u->sysfs_entry, name);
	vfs_entry_link(vfs_entry_directory(driver_bus_entry(DEVICE_BUS_USB),
					   "devices"),
		       name, d->sysfs_entry);
	vfs_entry_link(d->sysfs_entry, "subsystem",
		       driver_bus_entry(DEVICE_BUS_USB));
	vfs_entry_text(d->sysfs_entry, "uevent", "DEVTYPE=usb_interface\n");
	sprintf(text, "%04x\n", u->vendor);
	vfs_entry_text(d->sysfs_entry, "idVendor", text);
	sprintf(text, "%04x\n", u->product);
	vfs_entry_text(d->sysfs_entry, "idProduct", text);
	sprintf(text, "%02x\n", d->type);
	vfs_entry_text(d->sysfs_entry, "bInterfaceClass", text);
	sprintf(text, "%02x\n", interface->subclass);
	vfs_entry_text(d->sysfs_entry, "bInterfaceSubClass", text);
	sprintf(text, "%02x\n", interface->protocol);
	vfs_entry_text(d->sysfs_entry, "bInterfaceProtocol", text);
	sprintf(text, "%02x\n", interface->number);
	vfs_entry_text(d->sysfs_entry, "bInterfaceNumber", text);
	sprintf(text, "%u\n", interface->endpoint_count);
	vfs_entry_text(d->sysfs_entry, "bNumEndpoints", text);
	sprintf(text, "%u\n", u->busnum);
	vfs_entry_text(d->sysfs_entry, "busnum", text);
	sprintf(text, "%u\n", u->address);
	vfs_entry_text(d->sysfs_entry, "devnum", text);
	sprintf(text, "%s\n", u->path);
	vfs_entry_text(d->sysfs_entry, "devpath", text);
	sprintf(text, "%u\n", u->configuration);
	vfs_entry_text(d->sysfs_entry, "bConfigurationValue", text);
	sprintf(text, "%04x\n", u->usb_version);
	vfs_entry_text(d->sysfs_entry, "bcdUSB", text);
	sprintf(text, "%02x\n", u->device_class);
	vfs_entry_text(d->sysfs_entry, "bDeviceClass", text);
	const char *speed = u->speed == USB_SPEED_LOW  ? "1.5\n" :
			    u->speed == USB_SPEED_FULL ? "12\n" :
			    u->speed == USB_SPEED_HIGH ? "480\n" :
							 "5000\n";
	vfs_entry_text(d->sysfs_entry, "speed", speed);
	for (unsigned i = 0; i < interface->endpoint_count; i++) {
		struct usb_endpoint *ep = interface->endpoints[i];
		sprintf(text, "ep_%02x", ep->address);
		struct vfs_entry_node *entry =
			vfs_entry_directory(d->sysfs_entry, text);
		sprintf(text, "%02x\n", ep->address);
		vfs_entry_text(entry, "bEndpointAddress", text);
		sprintf(text, "%02x\n", ep->attributes);
		vfs_entry_text(entry, "bmAttributes", text);
		sprintf(text, "%04x\n",
			ep->max_packet |
				(u->speed == USB_SPEED_HIGH ? ep->burst << 11 :
							      0));
		vfs_entry_text(entry, "wMaxPacketSize", text);
		sprintf(text, "%u\n", ep->interval);
		vfs_entry_text(entry, "bInterval", text);
	}

	device_register(d);
	d->selected_driver = driver_select(d);
	d->match_done = 1;
	driver_bind(d);
	printk("usb: %04x:%04x interface=%u class=%02x subclass=%02x protocol=%02x driver=%s result=%d\n",
	       u->vendor, u->product, interface->number, d->type,
	       interface->subclass, interface->protocol,
	       d->selected_driver ? d->selected_driver->name : "unbound",
	       d->probe_error);
}
static int enumerate_device(struct usb_device *u)
{
	struct usb_device_descriptor device;
	struct usb_configuration_descriptor header;
	int ret = descriptor_read(u, USB_DESC_DEVICE, &device,
				  USB_EP0_DESCRIPTOR_PREFIX);
	if (ret != USB_EP0_DESCRIPTOR_PREFIX ||
	    device.header.length != sizeof(device) ||
	    device.header.type != USB_DESC_DEVICE)
		return -EIO;
	unsigned packet =
		u->speed >= USB_SPEED_SUPER ?
			(device.ep0_packet == USB_EP0_PACKET_SUPER_EXPONENT ?
				 USB_EP0_PACKET_SUPER :
				 0) :
			device.ep0_packet;
	if (packet != 8 && packet != 16 && packet != 32 && packet != 64 &&
	    packet != USB_EP0_PACKET_SUPER)
		return -EINVAL;
	ret = u->ops->update_ep0(u, packet);
	if (ret < 0)
		return ret;
	ret = descriptor_read(u, USB_DESC_DEVICE, &device, sizeof(device));
	if (ret != sizeof(device) || !device.configurations)
		return -EIO;
	u->vendor = device.vendor;
	u->product = device.product;
	u->usb_version = device.usb_version;
	u->device_class = device.device_class;
	ret = descriptor_read(u, USB_DESC_CONFIGURATION, &header,
			      sizeof(header));
	if (ret != sizeof(header) || header.header.length != sizeof(header) ||
	    header.header.type != USB_DESC_CONFIGURATION)
		return -EIO;
	unsigned length = header.total_length;
	if (length < sizeof(header) || length > USB_TRANSFER_MAX ||
	    !header.value)
		return -EINVAL;
	unsigned char *config = malloc(length);
	u->configuration_data = config;
	if (!config)
		return -ENOMEM;
	ret = descriptor_read(u, USB_DESC_CONFIGURATION, config, length);
	if (ret != (int)length || memcmp(&header, config, sizeof(header))) {
		ret = -EIO;
		goto out;
	}
	u->configuration = header.value;
	ret = parse_interfaces(u, config, length);
	if (ret < 0)
		goto out;
	ret = u->ops->configure(u);
	if (ret < 0)
		goto out;
	ret = usb_control(u, USB_DIR_OUT | USB_RECIP_DEVICE,
			  USB_REQ_SET_CONFIGURATION, u->configuration, 0, NULL,
			  0);
	if (ret < 0)
		goto out;
	for (unsigned i = 0; i < u->interface_count; i++)
		interface_register(&u->interfaces[i]);
	ret = 0;
out:
	return ret;
}
int usb_enumerate(struct usb_device *u)
{
	mutex_lock(&lifecycle_lock);
	int result = enumerate_device(u);
	mutex_unlock(&lifecycle_lock);
	return result;
}
void usb_disconnect(struct usb_device *u)
{
	u->online = 0;
	mutex_lock(&lifecycle_lock);
	for (unsigned i = 0; i < u->interface_count; i++) {
		struct usb_interface *interface = &u->interfaces[i];
		device_t *device = &interface->device;
		if (!device->match_done)
			continue;
		driver_t *driver = device->driver;
		if (driver && driver->disconnect_usb)
			driver->disconnect_usb(interface);
		char name[64];
		if (device->sysfs_entry) {
			strncpy(name, vfs_entry_name(device->sysfs_entry),
				sizeof(name));
			name[sizeof(name) - 1] = 0;
			if (driver)
				vfs_entry_remove(driver->sysfs_entry, name);
			vfs_entry_remove(
				vfs_entry_child(driver_bus_entry(DEVICE_BUS_USB),
						"devices"),
				name);
		}
		device_unregister(device);
		device->sysfs_entry = NULL;
		device->driver = device->selected_driver = NULL;
		device->match_done = 0;
	}
	if (u->sysfs_entry) {
		char name[48];
		sprintf(name, "%u-%s", u->busnum, u->path);
		vfs_entry_remove(
			vfs_entry_child(driver_bus_entry(DEVICE_BUS_USB),
					"devices"),
			name);
		vfs_entry_node *parent =
			u->parent ?
				u->parent->sysfs_entry :
				vfs_entry_child(
					vfs_entry_root(device_sysfs_entries()),
					"devices");
		vfs_entry_remove(parent, name);
		u->sysfs_entry = NULL;
	}
	mutex_unlock(&lifecycle_lock);
}
