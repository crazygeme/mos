#ifndef MOS_USB_H
#define MOS_USB_H
#include <driver/driver.h>
#include <lib/lock.h>
#include "protocol.h"
#include <lib/ref_count.h>

#define USB_MAX_INTERFACES 8
#define USB_MAX_ENDPOINTS 30
#define USB_TRANSFER_MAX 4096

struct usb_device;
struct usb_endpoint {
	unsigned address, attributes, max_packet, interval, burst,
		bytes_interval, mult;
	void *private;
	void (*receive)(struct usb_endpoint *, const unsigned char *, unsigned);
	void (*tick)(struct usb_endpoint *);
	void *class_data;
};
struct usb_alternate {
	struct usb_alternate *next;
	unsigned number, endpoint_count;
	const unsigned char *descriptors;
	unsigned descriptor_length;
	struct usb_endpoint endpoints[USB_MAX_ENDPOINTS];
};
struct usb_iso_packet {
	unsigned offset, length, actual;
	int status;
};
typedef void (*usb_iso_complete)(void *, int);
#define USB_ISO_PACKETS 256
#define USB_ISO_BUFFER_MAX (1024 * 1024)
struct usb_interface {
	device_t device;
	struct usb_device *usb;
	unsigned number, subclass, protocol, endpoint_count;
	struct usb_endpoint *endpoints[USB_MAX_ENDPOINTS];
	void *class_data;
	struct usb_alternate *alternates, *active;
};
struct usb_host_ops {
	int (*control)(struct usb_device *, unsigned, unsigned, unsigned,
		       unsigned, void *, unsigned);
	int (*transfer)(struct usb_device *, struct usb_endpoint *, void *,
			unsigned);
	int (*interrupt_start)(struct usb_device *, struct usb_endpoint *,
			       void (*)(struct usb_endpoint *,
					const unsigned char *, unsigned));
	int (*clear_halt)(struct usb_device *, struct usb_endpoint *);
	int (*configure)(struct usb_device *);
	int (*interface_stop)(struct usb_device *, struct usb_interface *);
	int (*iso_submit)(struct usb_device *, struct usb_endpoint *, void *,
			  struct usb_iso_packet *, unsigned, usb_iso_complete,
			  void *);
	int (*update_ep0)(struct usb_device *, unsigned);
	int (*hub_configure)(struct usb_device *, unsigned, unsigned);
	int (*child_attach)(struct usb_device *, unsigned, unsigned);
	void (*child_detach)(struct usb_device *, unsigned);
};
struct usb_device {
	ref_count_t references;
	const struct usb_host_ops *ops;
	void *host;
	unsigned slot, port, speed, online, vendor, product, configuration;
	struct usb_device *parent;
	unsigned hub_port, route, depth, tt_slot, tt_port;
	unsigned hub_ports, hub_think_time;
	unsigned busnum, address, usb_version, device_class;
	char path[32], sysfs_path[160];
	struct vfs_entry_node *sysfs_entry;
	unsigned interface_count, endpoint_count;
	struct usb_interface interfaces[USB_MAX_INTERFACES];
	struct usb_endpoint *endpoints[USB_MAX_ENDPOINTS];
	unsigned char *configuration_data;
	mutex_t lock, control_lock;
};

/* Controller operations are serialized internally. Class protocols retain the
 * device lock across command/data/status sequences. */
int usb_control(struct usb_device *, unsigned type, unsigned request,
		unsigned value, unsigned index, void *data, unsigned length);
int usb_transfer(struct usb_device *, struct usb_endpoint *, void *, unsigned);
int usb_interrupt_start(struct usb_device *, struct usb_endpoint *,
			void (*)(struct usb_endpoint *, const unsigned char *,
				 unsigned));
int usb_clear_halt(struct usb_device *, struct usb_endpoint *);
int usb_enumerate(struct usb_device *);
void usb_disconnect(struct usb_device *);
int usb_hid_set_leds(unsigned);
void usb_hub_poll(struct usb_device *);
int usb_set_interface(struct usb_interface *, unsigned);
int usb_iso_submit(struct usb_device *, struct usb_endpoint *, void *,
		   struct usb_iso_packet *, unsigned, usb_iso_complete, void *);
void usb_device_cleanup(struct usb_device *);

/* Shared console input adapters. */
int keyboard_input_init(void);
void keyboard_input_start(void);
void keyboard_input_byte(unsigned char);
int mouse_input_init(void);
void mouse_input_report(int dx, int dy, int wheel, unsigned buttons);
#endif
