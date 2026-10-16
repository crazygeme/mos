#ifndef MOS_XHCI_H
#define MOS_XHCI_H
#include <stdint.h>

/* xHCI capability, operational, and interrupter-zero register offsets. */
enum xhci_register {
	XHCI_CAP_HEADER = 0x00,
	XHCI_HCSPARAMS1 = 0x04,
	XHCI_HCSPARAMS2 = 0x08,
	XHCI_HCCPARAMS1 = 0x10,
	XHCI_DBOFF = 0x14,
	XHCI_RTSOFF = 0x18,
	XHCI_USBCMD = 0x00,
	XHCI_USBSTS = 0x04,
	XHCI_PAGESIZE = 0x08,
	XHCI_CRCR = 0x18,
	XHCI_DCBAAP = 0x30,
	XHCI_CONFIG = 0x38,
	XHCI_PORT_BASE = 0x400,
	XHCI_PORT_STRIDE = 0x10,
	XHCI_IMAN = 0x20,
	XHCI_ERSTSZ = 0x28,
	XHCI_ERSTBA = 0x30,
	XHCI_ERDP = 0x38,
	XHCI_RUNTIME_MIN_SIZE = 0x40,
};
enum xhci_register_bits {
	XHCI_CMD_RUN = 1u << 0,
	XHCI_CMD_RESET = 1u << 1,
	XHCI_STS_HALTED = 1u << 0,
	XHCI_STS_PORT_CHANGE = 1u << 4,
	XHCI_STS_NOT_READY = 1u << 11,
	XHCI_PAGE_4K = 1u << 0,
	XHCI_CONTEXT_64 = 1u << 2,
	XHCI_PORT_CONNECTED = 1u << 0,
	XHCI_PORT_ENABLED = 1u << 1,
	XHCI_PORT_RESET = 1u << 4,
	XHCI_PORT_POWER = 1u << 9,
	XHCI_PORT_CONNECT_CHANGE = 1u << 17,
	XHCI_PORT_WRITABLE = (1u << 9) | (3u << 14) | (7u << 25),
	XHCI_PORT_CHANGE = 0x00fe0000,
	XHCI_ERDP_BUSY = 1u << 3,
	XHCI_LEGACY_CAP_ID = 1,
	XHCI_LEGACY_BIOS_OWNED = 1u << 16,
	XHCI_LEGACY_OS_OWNED = 1u << 24,
	XHCI_PCI_MEMORY_BUSMASTER = (1u << 1) | (1u << 2),
	XHCI_PCI_IO_RESOURCE = 1u << 0,
};
enum xhci_trb_type {
	XHCI_TRB_NORMAL = 1,
	XHCI_TRB_SETUP = 2,
	XHCI_TRB_DATA = 3,
	XHCI_TRB_STATUS = 4,
	XHCI_TRB_ISOCH = 5,
	XHCI_TRB_LINK = 6,
	XHCI_TRB_EVENT_DATA = 7,
	XHCI_CMD_ENABLE_SLOT = 9,
	XHCI_CMD_DISABLE_SLOT = 10,
	XHCI_CMD_ADDRESS_DEVICE = 11,
	XHCI_CMD_CONFIGURE_ENDPOINT = 12,
	XHCI_CMD_EVALUATE_CONTEXT = 13,
	XHCI_CMD_RESET_ENDPOINT = 14,
	XHCI_CMD_STOP_ENDPOINT = 15,
	XHCI_CMD_SET_DEQUEUE = 16,
	XHCI_EVENT_TRANSFER = 32,
	XHCI_EVENT_COMMAND = 33,
	XHCI_EVENT_PORT_CHANGE = 34,
};
enum xhci_completion {
	XHCI_CC_SUCCESS = 1,
	XHCI_CC_STALL = 6,
	XHCI_CC_SHORT_PACKET = 13,
};
enum xhci_trb_bits {
	XHCI_TRB_CYCLE = 1u << 0,
	XHCI_TRB_TOGGLE_CYCLE = 1u << 1,
	XHCI_TRB_SHORT_EVENT = 1u << 2,
	XHCI_EVENT_HAS_DATA = 1u << 2,
	XHCI_TRB_CHAIN = 1u << 4,
	XHCI_TRB_IOC = 1u << 5,
	XHCI_TRB_IMMEDIATE = 1u << 6,
	XHCI_TRB_DIR_IN = 1u << 16,
	XHCI_SETUP_NO_DATA = 0,
	XHCI_SETUP_DATA_OUT = 2u << 16,
	XHCI_SETUP_DATA_IN = 3u << 16,
};
enum xhci_endpoint_state { XHCI_EP_RUNNING = 1, XHCI_EP_HALTED = 2 };
enum xhci_endpoint_type {
	XHCI_EP_ISOCH_OUT = 1,
	XHCI_EP_BULK_OUT = 2,
	XHCI_EP_INTERRUPT_OUT = 3,
	XHCI_EP_CONTROL = 4,
	XHCI_EP_ISOCH_IN = 5,
	XHCI_EP_BULK_IN = 6,
	XHCI_EP_INTERRUPT_IN = 7,
};
#define XHCI_PCI_USB_CLASS 0x0c03
#define XHCI_PCI_CLASS_MASK 0xffff
#define XHCI_PCI_PROGIF 0x30
#define XHCI_ENDPOINTS 32
#define XHCI_DCI_CONTROL 1
#define XHCI_INPUT_CONTROL_INDEX 0
#define XHCI_INPUT_SLOT_INDEX 1
#define XHCI_INPUT_EP_INDEX(dci) ((dci) + 1)
#define XHCI_CONTEXT_SMALL_SIZE 32
#define XHCI_CONTEXT_LARGE_SIZE 64
#define XHCI_DWORD_BYTES 4
#define XHCI_COMMAND_TIMEOUT_MS 1000
#define XHCI_TRANSFER_TIMEOUT_MS 3000
#define XHCI_PORT_SETTLE_MS 20
#define XHCI_POLL_MS 2
#define XHCI_PORT_POLL_MS 50
#define XHCI_PORT_DEBOUNCE_MS 100
#define XHCI_MAX_CAPABILITIES 256
#define XHCI_MAX_PORTS 255
#define XHCI_PERIODIC_MAX_EXPONENT 15
#define XHCI_FULL_SPEED_MICROFRAME_EXPONENT 3
#define XHCI_MAX_ENDPOINT_PACKET 1024
#define XHCI_EP_STATE_MASK 7u
#define XHCI_EP_PACKET_MASK 0xffff0000u
#define XHCI_SLOT_ENTRIES_MASK (31u << 27)
#define XHCI_EP_ERROR_RETRIES (3u << 1)
#define XHCI_TRB_RESIDUAL_MASK 0x00ffffffu
#define XHCI_TRB_TYPE(type) ((type) << 10)
#define XHCI_TRB_SLOT(slot) ((unsigned)(slot) << 24)
#define XHCI_TRB_ENDPOINT(dci) ((dci) << 16)
#define XHCI_EP_PACKET(packet) ((packet) << 16)
#define XHCI_EP_TYPE(type) ((type) << 3)
#define XHCI_EP_INTERVAL(interval) ((interval) << 16)
#define XHCI_EP_BURST(burst) (((burst) & 15) << 8)
#define XHCI_EP_ESIT(bytes) ((bytes) << 16)
#define XHCI_SLOT_SPEED(speed) ((speed) << 20)
#define XHCI_SLOT_ENTRIES(count) ((count) << 27)
#define XHCI_SLOT_PORT(port) ((port) << 16)
#define XHCI_CONTEXT_FLAG(index) (1u << (index))

struct xhci_input_control {
	uint32_t drop_flags, add_flags;
};
struct xhci_slot_context {
	uint32_t route_speed_entries, port_latency, tt, address_state;
};
struct xhci_endpoint_context {
	uint32_t state_interval, type_packet;
	uint64_t dequeue;
	uint32_t average_esit;
};
struct usb_setup_packet {
	uint8_t type, request;
	uint16_t value, index, length;
} __attribute__((packed));
struct xhci_erst_entry {
	uint64_t address;
	uint32_t size, reserved;
};
_Static_assert(sizeof(struct usb_setup_packet) == 8, "USB setup packet size");

static inline unsigned xhci_port_offset(unsigned port)
{
	return XHCI_PORT_BASE + (port - 1) * XHCI_PORT_STRIDE;
}
static inline unsigned xhci_doorbell_offset(unsigned slot)
{
	return slot * XHCI_DWORD_BYTES;
}
static inline unsigned xhci_event_type(unsigned control)
{
	return (control >> 10) & 63;
}
static inline unsigned xhci_event_slot(unsigned control)
{
	return control >> 24;
}
static inline unsigned xhci_event_endpoint(unsigned control)
{
	return (control >> 16) & 31;
}
static inline unsigned xhci_completion_code(unsigned status)
{
	return status >> 24;
}
static inline unsigned xhci_port_speed(unsigned status)
{
	return (status >> 10) & 15;
}
static inline unsigned xhci_cap_length(unsigned cap)
{
	return cap & 255;
}
static inline unsigned xhci_cap_ports(unsigned params)
{
	return params >> 24;
}
static inline unsigned xhci_cap_slots(unsigned params)
{
	return params & 255;
}
static inline unsigned xhci_scratchpad_count(unsigned params)
{
	return ((params >> 27) & 31) | ((params >> 16) & 0x3e0);
}
static inline unsigned xhci_extended_cap_offset(unsigned params)
{
	return (params >> 16) * XHCI_DWORD_BYTES;
}
static inline unsigned xhci_extended_cap_id(unsigned cap)
{
	return cap & 255;
}
static inline unsigned xhci_extended_cap_next(unsigned cap)
{
	return ((cap >> 8) & 255) * XHCI_DWORD_BYTES;
}
static inline unsigned xhci_doorbell_base(unsigned value)
{
	return value & ~3u;
}
static inline unsigned xhci_runtime_base(unsigned value)
{
	return value & ~31u;
}
#endif
