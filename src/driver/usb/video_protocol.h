#ifndef MOS_USB_VIDEO_PROTOCOL_H
#define MOS_USB_VIDEO_PROTOCOL_H
#include <stdint.h>
/* Fixed-width V4L2 wire fields; timeval and user pointers follow the native ABI. */
#define UVC_YUYV 0x56595559u
#define UVC_MJPEG 0x47504a4du
#define UVC_BUFFERS 4
#define UVC_FORMATS 32
#define UVC_FRAME_MAX (4 * 1024 * 1024)
struct video_capability {
	char driver[16], card[32], bus[32];
	uint32_t version, capabilities, device_caps, reserved[3];
};
struct video_pix_format {
	uint32_t width, height, format, field, stride, size, colorspace;
	uint32_t private, flags, encoding, quantization, transfer;
};
struct video_format {
	uint32_t type;
	union {
		struct video_pix_format pix;
		uint64_t alignment;
		char raw[200];
	} data;
};
struct video_format_desc {
	uint32_t index, type, flags;
	char description[32];
	uint32_t format, mbus, reserved[3];
};
struct video_request_buffers {
	uint32_t count, type, memory, capabilities, flags;
};
struct video_timecode {
	uint32_t type, flags;
	uint8_t frames, seconds, minutes, hours, bits[4];
};
struct video_buffer {
	uint32_t index, type, bytes, flags, field;
	intptr_t seconds, microseconds;
	struct video_timecode timecode;
	uint32_t sequence, memory;
	union {
		uint32_t offset;
		uintptr_t pointer;
		int32_t fd;
	} location;
	uint32_t length, reserved, request;
};
struct video_input {
	uint32_t index;
	char name[32];
	uint32_t type, audio, tuner;
	uint64_t standard;
	uint32_t status, capabilities, reserved[3];
};
struct video_stream_parameters {
	uint32_t type;
	union {
		struct {
			uint32_t capabilities, mode, numerator, denominator,
				extended, buffers, reserved[4];
		} capture;
		char raw[200];
	} data;
};
struct video_frame_size {
	uint32_t index, format, type;
	union {
		struct {
			uint32_t width, height;
		} discrete;
		uint32_t stepwise[6];
	} size;
	uint32_t reserved[2];
};
struct video_frame_interval {
	uint32_t index, format, width, height, type;
	union {
		struct {
			uint32_t numerator, denominator;
		} discrete;
		uint32_t stepwise[6];
	} interval;
	uint32_t reserved[2];
};
_Static_assert(sizeof(struct video_capability) == 104, "V4L2 capabilities ABI");
_Static_assert(sizeof(struct video_format_desc) == 64,
	       "V4L2 format description ABI");
_Static_assert(sizeof(struct video_request_buffers) == 20,
	       "V4L2 request buffers ABI");
_Static_assert(sizeof(struct video_frame_size) == 44, "V4L2 frame size ABI");
_Static_assert(sizeof(struct video_frame_interval) == 52,
	       "V4L2 frame interval ABI");
#endif
