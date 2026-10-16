#ifndef MOS_USB_PROTOCOL_H
#define MOS_USB_PROTOCOL_H
#include <stdint.h>

/* USB 2.0 chapter 9 and USB 3.x standard descriptor fields. */
enum usb_request_type {
	USB_DIR_OUT = 0x00,
	USB_DIR_IN = 0x80,
	USB_TYPE_STANDARD = 0x00,
	USB_TYPE_CLASS = 0x20,
	USB_RECIP_DEVICE = 0x00,
	USB_RECIP_INTERFACE = 0x01,
	USB_RECIP_ENDPOINT = 0x02,
	USB_RECIP_OTHER = 0x03,
};
enum usb_request {
	USB_REQ_GET_STATUS = 0,
	USB_REQ_CLEAR_FEATURE = 1,
	USB_REQ_SET_FEATURE = 3,
	USB_REQ_GET_DESCRIPTOR = 6,
	USB_REQ_SET_CONFIGURATION = 9,
	USB_REQ_SET_INTERFACE = 11,
};
enum usb_descriptor_type {
	USB_DESC_DEVICE = 1,
	USB_DESC_CONFIGURATION = 2,
	USB_DESC_INTERFACE = 4,
	USB_DESC_ENDPOINT = 5,
	USB_DESC_SS_ENDPOINT_COMPANION = 48,
};
enum usb_class {
	USB_CLASS_AUDIO = 1,
	USB_CLASS_HID = 3,
	USB_CLASS_STORAGE = 8,
	USB_CLASS_HUB = 9
};
enum usb_speed {
	USB_SPEED_FULL = 1,
	USB_SPEED_LOW = 2,
	USB_SPEED_HIGH = 3,
	USB_SPEED_SUPER = 4,
};
enum usb_transfer_type {
	USB_XFER_CONTROL = 0,
	USB_XFER_ISOCHRONOUS = 1,
	USB_XFER_BULK = 2,
	USB_XFER_INTERRUPT = 3,
};
#define USB_FEATURE_ENDPOINT_HALT 0
#define USB_ENDPOINT_NUMBER_MASK 0x0f
#define USB_ENDPOINT_RESERVED_MASK 0x70
#define USB_ENDPOINT_TYPE_MASK 0x03
#define USB_ENDPOINT_PACKET_MASK 0x07ff
#define USB_ENDPOINT_TRANSACTIONS_MASK 0x1800
#define USB_EP0_DESCRIPTOR_PREFIX 8
#define USB_EP0_PACKET_LOW 8
#define USB_EP0_PACKET_FULL_MAX 64
#define USB_EP0_PACKET_SUPER 512
#define USB_EP0_PACKET_SUPER_EXPONENT 9
#define USB_DEFAULT_ALTERNATE 0
#define USB_DEFAULT_LUN 0
#define USB_DESCRIPTOR_VALUE(type, index) (((type) << 8) | (index))

struct usb_descriptor_header {
	uint8_t length, type;
} __attribute__((packed));
struct usb_device_descriptor {
	struct usb_descriptor_header header;
	uint16_t usb_version;
	uint8_t device_class, device_subclass, device_protocol, ep0_packet;
	uint16_t vendor, product, device_version;
	uint8_t manufacturer, product_string, serial_string, configurations;
} __attribute__((packed));
struct usb_configuration_descriptor {
	struct usb_descriptor_header header;
	uint16_t total_length;
	uint8_t interfaces, value, string, attributes, max_power;
} __attribute__((packed));
struct usb_interface_descriptor {
	struct usb_descriptor_header header;
	uint8_t number, alternate, endpoints, interface_class, subclass,
		protocol, string;
} __attribute__((packed));
struct usb_endpoint_descriptor {
	struct usb_descriptor_header header;
	uint8_t address, attributes;
	uint16_t max_packet;
	uint8_t interval;
} __attribute__((packed));
struct usb_ss_endpoint_descriptor {
	struct usb_descriptor_header header;
	uint8_t max_burst, attributes;
	uint16_t bytes_per_interval;
} __attribute__((packed));
_Static_assert(sizeof(struct usb_device_descriptor) == 18,
	       "USB device descriptor size");
_Static_assert(sizeof(struct usb_configuration_descriptor) == 9,
	       "USB configuration descriptor size");
_Static_assert(sizeof(struct usb_endpoint_descriptor) == 7,
	       "USB endpoint descriptor size");

/* MOS targets little-endian x86; SCSI command fields use big-endian order. */
static inline uint32_t usb_get_be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}
static inline void usb_put_be32(unsigned char *p, uint32_t value)
{
	p[0] = value >> 24;
	p[1] = value >> 16;
	p[2] = value >> 8;
	p[3] = value;
}
static inline uint32_t usb_get_le32(const unsigned char *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
#endif
