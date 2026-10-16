#ifndef MOS_USB_STORAGE_H
#define MOS_USB_STORAGE_H
#include <stdint.h>

/* USB Mass Storage Bulk-Only Transport 1.0 and SCSI block commands. */
enum usb_storage_request {
	USB_STORAGE_GET_MAX_LUN = 0xfe,
	USB_STORAGE_RESET = 0xff
};
enum usb_storage_interface {
	USB_STORAGE_SCSI = 6,
	USB_STORAGE_BULK_ONLY = 0x50
};
enum usb_storage_status {
	USB_STORAGE_PASSED = 0,
	USB_STORAGE_FAILED = 1,
	USB_STORAGE_PHASE_ERROR = 2
};
enum scsi_opcode {
	SCSI_TEST_UNIT_READY = 0x00,
	SCSI_REQUEST_SENSE = 0x03,
	SCSI_INQUIRY = 0x12,
	SCSI_READ_CAPACITY10 = 0x25,
	SCSI_READ10 = 0x28,
	SCSI_WRITE10 = 0x2a,
	SCSI_SYNCHRONIZE_CACHE10 = 0x35,
};
#define USB_STORAGE_CBW_SIGNATURE 0x43425355u
#define USB_STORAGE_CSW_SIGNATURE 0x53425355u
#define USB_STORAGE_SECTOR_SIZE 512u
#define USB_STORAGE_MINORS_PER_DISK 16u
#define USB_STORAGE_READY_ATTEMPTS 5
#define SCSI_DEVICE_TYPE_MASK 0x1f
#define SCSI_DEVICE_DIRECT_ACCESS 0
#define SCSI_READ10_MAX_LBA 0xffffffffULL
#define MBR_SIGNATURE 0xaa55
#define MBR_PRIMARY_PARTITIONS 4
#define MBR_TYPE_EXTENDED_CHS 0x05
#define MBR_TYPE_EXTENDED_LBA 0x0f
#define MBR_TYPE_PROTECTIVE_GPT 0xee

struct usb_storage_cbw {
	uint32_t signature, tag, length;
	uint8_t flags, lun, cdb_length, cdb[16];
} __attribute__((packed));
struct usb_storage_csw {
	uint32_t signature, tag, residue;
	uint8_t status;
} __attribute__((packed));
struct scsi_command6 {
	uint8_t opcode, flags, address_high, address_low, allocation_length,
		control;
} __attribute__((packed));
struct scsi_command10 {
	uint8_t opcode, flags, lba[4], group, count[2], control;
} __attribute__((packed));
struct scsi_inquiry_data {
	uint8_t device_type, removable, version, flags, length, reserved[3];
	char vendor[8], product[16], revision[4];
} __attribute__((packed));
struct scsi_sense_data {
	uint8_t bytes[18];
};
struct scsi_capacity10 {
	uint8_t last_lba[4], block_size[4];
};
struct mbr_partition {
	uint8_t bootable, start_chs[3], type, end_chs[3], start[4], sectors[4];
} __attribute__((packed));
struct mbr_sector {
	uint8_t loader[446];
	struct mbr_partition partitions[MBR_PRIMARY_PARTITIONS];
	uint16_t signature;
} __attribute__((packed));
_Static_assert(sizeof(struct usb_storage_cbw) == 31, "BOT CBW size");
_Static_assert(sizeof(struct usb_storage_csw) == 13, "BOT CSW size");
_Static_assert(sizeof(struct scsi_command6) == 6, "SCSI six-byte command size");
_Static_assert(sizeof(struct scsi_command10) == 10,
	       "SCSI ten-byte command size");
_Static_assert(sizeof(struct mbr_sector) == USB_STORAGE_SECTOR_SIZE,
	       "MBR sector size");
#endif
