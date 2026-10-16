/* USB hub class port management; child enumeration runs outside transport locks. */
#include <device/usb/usb.h>
#include <lib/klib.h>
#include <errno.h>
#include <macro.h>
#include <ps/ps.h>

#define HUB_DESC_USB2 0x29
#define HUB_DESC_SUPER 0x2a
#define HUB_SET_DEPTH 12
#define HUB_PORT_ENABLE 1
#define HUB_PORT_RESET 4
#define HUB_PORT_POWER 8
#define HUB_PORT_CONNECTION (1u << 0)
#define HUB_PORT_ENABLED (1u << 1)
#define HUB_PORT_OVERCURRENT (1u << 3)
#define HUB_PORT_RESETTING (1u << 4)
#define HUB_PORT_LOW_SPEED (1u << 9)
#define HUB_PORT_HIGH_SPEED (1u << 10)
#define HUB_CHANGE_CONNECTION (1u << 16)
#define HUB_CHANGE_OVERCURRENT (1u << 19)
#define HUB_MAX_PORTS 255
#define HUB_BITMAP_BYTES 32
#define HUB_POLL_MS 1000
#define HUB_RESET_TIMEOUT_MS 1000

struct hub {
	struct usb_device *usb;
	struct usb_endpoint *interrupt;
	unsigned ports, power_delay;
	unsigned char attached[HUB_MAX_PORTS + 1];
	unsigned char changes[HUB_BITMAP_BYTES];
	uint64_t next_poll;
};
static int hub_status(struct hub *hub, unsigned port, uint32_t *status)
{
	int ret = usb_control(
		hub->usb,
		USB_DIR_IN | USB_TYPE_CLASS |
			(port ? USB_RECIP_OTHER : USB_RECIP_DEVICE),
		USB_REQ_GET_STATUS, 0, port, status, sizeof(*status));
	return ret == sizeof(*status) ? 0 : ret < 0 ? ret : -EIO;
}
static int hub_feature(struct hub *hub, unsigned port, unsigned feature,
		       int set)
{
	return usb_control(hub->usb,
			   USB_DIR_OUT | USB_TYPE_CLASS |
				   (port ? USB_RECIP_OTHER : USB_RECIP_DEVICE),
			   set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE,
			   feature, port, NULL, 0);
}
static int hub_acknowledge(struct hub *hub, unsigned port, uint32_t status)
{
	for (unsigned bit = 0; bit < 8; bit++) {
		if (!(status & (1u << (bit + 16))))
			continue;
		unsigned feature = bit + 16;
		if (hub->usb->speed >= USB_SPEED_SUPER) {
			/* USB3 warm-reset change occupies bit 5, selector 29. */
			if (bit == 5)
				feature = 29;
			else if (bit == 1 || bit == 2)
				continue;
		} else if (bit > 4)
			continue;
		if (hub_feature(hub, port, feature, 0) < 0)
			return -EIO;
	}
	return 0;
}
static void hub_report(struct usb_endpoint *ep, const unsigned char *data,
		       unsigned length)
{
	struct hub *hub = ep->class_data;
	if (length > sizeof(hub->changes))
		length = sizeof(hub->changes);
	for (unsigned i = 0; i < length; i++)
		hub->changes[i] |= data[i];
}
static void hub_detach_port(struct hub *hub, unsigned port)
{
	if (hub->attached[port]) {
		hub->usb->ops->child_detach(hub->usb, port);
		hub->attached[port] = 0;
	}
}
static int hub_reset_port(struct hub *hub, unsigned port, uint32_t *status)
{
	if (hub_feature(hub, port, HUB_PORT_RESET, 1) < 0)
		return -EIO;
	uint64_t deadline = time_deadline_ms(HUB_RESET_TIMEOUT_MS);
	do {
		time_wait(20);
		if (hub_status(hub, port, status) < 0)
			return -EIO;
		if (!(*status & HUB_PORT_CONNECTION))
			return -ENODEV;
		if (!(*status & HUB_PORT_RESETTING) &&
		    (*status & HUB_PORT_ENABLED)) {
			if (hub_acknowledge(hub, port, *status) < 0)
				return -EIO;
			time_wait(20);
			return 0;
		}
	} while (time_now_ms() < deadline);
	return -ETIMEDOUT;
}
static void hub_poll_port(struct hub *hub, unsigned port)
{
	uint32_t status;
	if (hub_status(hub, port, &status) < 0)
		return;
	if (!(status & HUB_PORT_CONNECTION) || !(status & HUB_PORT_ENABLED) ||
	    (status & HUB_CHANGE_CONNECTION) || (status & HUB_PORT_OVERCURRENT))
		hub_detach_port(hub, port);
	if (hub_acknowledge(hub, port, status) < 0)
		return;
	if (status & HUB_PORT_OVERCURRENT) {
		hub_feature(hub, port, HUB_PORT_POWER, 0);
		return;
	}
	if (status & HUB_CHANGE_OVERCURRENT) {
		if (hub_feature(hub, port, HUB_PORT_POWER, 1) < 0)
			return;
		time_wait(hub->power_delay);
	}
	if (!(status & HUB_PORT_CONNECTION) || hub->attached[port])
		return;
	/* Require a stable connection before resetting and assigning a slot. */
	time_wait(100);
	if (hub_status(hub, port, &status) < 0 ||
	    !(status & HUB_PORT_CONNECTION) || (status & HUB_CHANGE_CONNECTION))
		return;
	int ret = hub_reset_port(hub, port, &status);
	if (ret < 0)
		return;
	unsigned speed = hub->usb->speed >= USB_SPEED_SUPER ? USB_SPEED_SUPER :
			 (status & HUB_PORT_HIGH_SPEED)	    ? USB_SPEED_HIGH :
			 (status & HUB_PORT_LOW_SPEED)	    ? USB_SPEED_LOW :
							      USB_SPEED_FULL;
	ret = hub->usb->ops->child_attach(hub->usb, port, speed);
	if (ret >= 0)
		hub->attached[port] = 1;
	else {
		if (hub->usb->speed < USB_SPEED_SUPER)
			hub_feature(hub, port, HUB_PORT_ENABLE, 0);
		printk("usb: hub slot=%u port=%u enumeration failed: %d\n",
		       hub->usb->slot, port, ret);
	}
}
void usb_hub_poll(struct usb_device *usb)
{
	for (unsigned i = 0; i < usb->interface_count && usb->online; i++) {
		struct usb_interface *interface = &usb->interfaces[i];
		if (interface->device.type != USB_CLASS_HUB ||
		    !interface->device.driver || !interface->class_data)
			continue;
		struct hub *hub = interface->class_data;
		int periodic = time_now_ms() >= hub->next_poll;
		if (periodic)
			hub->next_poll = time_deadline_ms(HUB_POLL_MS);
		if (periodic || (hub->changes[0] & 1)) {
			uint32_t status;
			hub->changes[0] &= ~1u;
			if (hub_status(hub, 0, &status) == 0) {
				for (unsigned bit = 0; bit < 2; bit++)
					if (status & (1u << (bit + 16)))
						hub_feature(hub, 0, bit, 0);
				if (status & 2) {
					for (unsigned port = 1;
					     port <= hub->ports; port++)
						hub_detach_port(hub, port);
					continue;
				}
			}
		}
		for (unsigned port = 1; port <= hub->ports && usb->online;
		     port++) {
			unsigned byte = port / 8, bit = 1u << (port % 8);
			if (!periodic && !(hub->changes[byte] & bit))
				continue;
			hub->changes[byte] &= ~bit;
			hub_poll_port(hub, port);
		}
	}
}
static void hub_disconnect(struct usb_interface *interface)
{
	struct hub *hub = interface->class_data;
	if (!hub)
		return;
	/* The host removes descendant slots before releasing the hub slot. */
	hub->interrupt->receive = NULL;
	hub->interrupt->class_data = NULL;
	interface->class_data = NULL;
	free(hub);
}
static int hub_probe(struct usb_interface *interface)
{
	struct usb_device *usb = interface->usb;
	if (!usb->ops->hub_configure || !usb->ops->child_attach ||
	    !usb->ops->child_detach || usb->depth >= 5)
		return -EOPNOTSUPP;
	struct usb_endpoint *interrupt = NULL;
	for (unsigned i = 0; i < interface->endpoint_count; i++) {
		struct usb_endpoint *ep = interface->endpoints[i];
		if ((ep->address & USB_DIR_IN) &&
		    (ep->attributes & USB_ENDPOINT_TYPE_MASK) ==
			    USB_XFER_INTERRUPT)
			interrupt = ep;
	}
	if (!interrupt)
		return -EINVAL;
	unsigned char descriptor[72];
	unsigned type = usb->speed >= USB_SPEED_SUPER ? HUB_DESC_SUPER :
							HUB_DESC_USB2;
	int ret = usb_control(usb,
			      USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
			      USB_REQ_GET_DESCRIPTOR,
			      USB_DESCRIPTOR_VALUE(type, 0), 0, descriptor,
			      sizeof(descriptor));
	if (ret < 7 || descriptor[1] != type || !descriptor[2] ||
	    descriptor[0] > ret ||
	    descriptor[0] < (type == HUB_DESC_SUPER ?
				     12 :
				     7 + ((descriptor[2] + 8) / 8)) ||
	    interrupt->max_packet < (descriptor[2] + 8) / 8)
		return ret < 0 ? ret : -EINVAL;
	struct hub *hub = zalloc(sizeof(*hub));
	if (!hub)
		return -ENOMEM;
	hub->usb = usb;
	hub->interrupt = interrupt;
	hub->ports = descriptor[2];
	hub->power_delay = descriptor[5] * 2 + 20;
	ret = usb->ops->hub_configure(usb, hub->ports,
				      (descriptor[3] >> 5) & 3);
	if (ret < 0)
		goto failed;
	if (usb->speed >= USB_SPEED_SUPER) {
		ret = usb_control(
			usb, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE,
			HUB_SET_DEPTH, usb->depth, 0, NULL, 0);
		if (ret < 0)
			goto failed;
	}
	for (unsigned port = 1; port <= hub->ports; port++) {
		ret = hub_feature(hub, port, HUB_PORT_POWER, 1);
		if (ret < 0)
			goto failed;
	}
	time_wait(hub->power_delay);
	interrupt->class_data = hub;
	interface->class_data = hub;
	ret = usb_interrupt_start(usb, interrupt, hub_report);
	if (ret < 0) {
		interrupt->class_data = NULL;
		interface->class_data = NULL;
		goto failed;
	}
	printk("usb: hub slot=%u ports=%u depth=%u configured\n", usb->slot,
	       hub->ports, usb->depth);
	return 0;
failed:
	free(hub);
	return ret;
}
static driver_t hub_driver = {
	.name = "usb-hub",
	.bus = DEVICE_BUS_USB,
	.usb_class = USB_CLASS_HUB,
	.probe_usb = hub_probe,
	.disconnect_usb = hub_disconnect,
};
DRIVER_REGISTER(hub_driver);
