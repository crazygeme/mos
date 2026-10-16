/* UVC capture with V4L2 read and shared mmap buffer queues. */
#include <device/usb/usb.h>
#include <device/devnode.h>
#include <device/devnums.h>
#include <fs/entries.h>
#include <fs/sysfs.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <ps/ps.h>
#include <errno.h>
#include <net/sock.h>
#include <macro.h>
#include "video_protocol.h"

#define UVC_CAMERAS 4
#define UVC_CAPTURE_CAPS (1u | (1u << 24) | (1u << 26))
struct capture_format {
	unsigned format, format_index, frame_index, width, height, size,
		interval;
};
struct capture_file;
struct camera {
	ref_count_t references;
	struct usb_interface *interface;
	struct capture_format formats[UVC_FORMATS];
	unsigned format_count, index, online;
	mutex_t lock;
	struct capture_file *owner;
};
struct capture_frame {
	void *data;
	unsigned state, bytes, sequence;
	uint64_t time;
};
struct capture_batch {
	struct capture_file *file;
	void *data;
	struct usb_iso_packet packets[USB_ISO_PACKETS];
	unsigned complete, submitted;
	int result;
};
struct capture_file {
	struct camera *camera;
	struct capture_format format;
	struct usb_endpoint *endpoint;
	struct capture_frame frames[UVC_BUFFERS];
	struct capture_batch batches[2];
	unsigned count, stride, streaming, stopping, worker, sequence;
	unsigned packet_size, packet_count, fid, synchronized, bad, accumulated;
	int filling, error;
	mutex_t lock;
	cond_t wake, stopped;
	list_entry waiters;
	spinlock_t wait_lock;
};
static struct camera *cameras[UVC_CAMERAS];
static spinlock_t cameras_lock = SPINLOCK_INITIALIZER;
static void video_notify(struct capture_file *state)
{
	int irq;
	spinlock_lock(&state->wait_lock, &irq);
	poll_notify(&state->waiters);
	spinlock_unlock(&state->wait_lock, irq);
}
static int video_user_range(const void *pointer, unsigned size)
{
	uintptr_t pos = (uintptr_t)pointer;
	if (!size || pos < PAGE_SIZE || pos >= KERNEL_OFFSET ||
	    size > KERNEL_OFFSET - pos)
		return 0;
	uintptr_t end = pos + size;
	while (pos < end) {
		vm_region *region = vm_find_map(current->memory, pos);
		if (!region || !(region->prot & PROT_WRITE))
			return 0;
		pos = region->end < end ? region->end : end;
	}
	return 1;
}
static uint32_t le32(const unsigned char *p)
{
	return usb_get_le32(p);
}
static unsigned le16(const unsigned char *p)
{
	return p[0] | (unsigned)p[1] << 8;
}
static void put32(unsigned char *p, unsigned value)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = value >> (i * 8);
}
static void camera_release(ref_count_t *ref)
{
	struct camera *camera = (void *)ref;
	ref_count_put(camera->interface->usb);
	free(camera);
}
static int parse_formats(struct camera *camera)
{
	struct usb_alternate *alt = camera->interface->active;
	unsigned format = 0, format_index = 0;
	for (unsigned offset = 0; offset + 2 <= alt->descriptor_length;) {
		const unsigned char *d = alt->descriptors + offset;
		if (d[0] < 2 || d[0] > alt->descriptor_length - offset)
			return -EINVAL;
		if (d[1] == 0x24 && d[0] >= 4) {
			if (d[2] == 4 || d[2] == 6) {
				format = d[2] == 6 ? UVC_MJPEG :
					 d[0] >= 27 && !memcmp(d + 5, "YUY2",
							       4) ?
						     UVC_YUYV :
						     0;
				format_index = d[3];
			} else if (format &&
				   ((d[2] == 5 && format == UVC_YUYV) ||
				    (d[2] == 7 && format == UVC_MJPEG)) &&
				   d[0] >= 26) {
				if (camera->format_count == UVC_FORMATS)
					return -ENOSPC;
				unsigned width = le16(d + 5),
					 height = le16(d + 7);
				if (!width || !height || width > 4096 ||
				    height > 4096 ||
				    (uint64_t)width * height * 2 >
					    UVC_FRAME_MAX)
					goto next;
				struct capture_format *f =
					&camera->formats[camera->format_count++];
				*f = (struct capture_format){
					format,
					format_index,
					d[3],
					width,
					height,
					format == UVC_YUYV ? width * height * 2 : le32(d + 17),
					le32(d + 21)
				};
				if (!f->size || f->size > UVC_FRAME_MAX)
					f->size = width * height * 2;
				/* Select the slowest discrete interval to bound bandwidth. */
				if (d[25] && d[0] >= 26 + d[25] * 4)
					f->interval =
						le32(d + 26 + (d[25] - 1) * 4);
				if (!f->interval)
					f->interval = 666666;
			}
		}
next:
		offset += d[0];
	}
	return camera->format_count ? 0 : -EOPNOTSUPP;
}
static int allocate_frames(struct capture_file *state, unsigned count)
{
	unsigned stride = (state->format.size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
	for (unsigned i = 0; i < state->count; i++) {
		vm_free((vaddr_t)state->frames[i].data,
			state->stride / PAGE_SIZE);
		memset(&state->frames[i], 0, sizeof(state->frames[i]));
	}
	state->count = 0;
	state->stride = stride;
	for (unsigned i = 0; i < count; i++) {
		state->frames[i].data = (void *)vm_alloc(stride / PAGE_SIZE);
		if (!state->frames[i].data)
			return -ENOMEM;
		memset(state->frames[i].data, 0, stride);
		state->count++;
	}
	return 0;
}
static int probe_stream(struct capture_file *state)
{
	struct usb_interface *interface = state->camera->interface;
	struct usb_device *usb = interface->usb;
	unsigned char probe[48] = { 0 }, size[2] = { 0 };
	int ret = usb_control(usb,
			      USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			      0x85, 0x100, interface->number, size, 2);
	unsigned length = ret == 2 ? le16(size) : 26;
	if (length < 26 || length > sizeof(probe))
		return -EOPNOTSUPP;
	probe[0] = 1;
	probe[2] = state->format.format_index;
	probe[3] = state->format.frame_index;
	put32(probe + 4, state->format.interval);
	put32(probe + 18, state->format.size);
	ret = usb_control(usb, USB_TYPE_CLASS | USB_RECIP_INTERFACE, 1, 0x100,
			  interface->number, probe, length);
	if (ret < 0)
		return ret;
	ret = usb_control(usb,
			  USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			  0x81, 0x100, interface->number, probe, length);
	if (ret != (int)length || probe[2] != state->format.format_index ||
	    probe[3] != state->format.frame_index)
		return -EIO;
	unsigned payload = le32(probe + 22);
	struct usb_alternate *selected = NULL;
	struct usb_endpoint *endpoint = NULL;
	unsigned capacity = ~0u;
	for (struct usb_alternate *a = interface->alternates; a; a = a->next)
		for (unsigned i = 0; i < a->endpoint_count; i++) {
			struct usb_endpoint *ep = &a->endpoints[i];
			unsigned bytes =
				ep->bytes_interval ?
					ep->bytes_interval :
					ep->max_packet * (ep->burst + 1);
			if ((ep->address & USB_DIR_IN) &&
			    (ep->attributes & USB_ENDPOINT_TYPE_MASK) ==
				    USB_XFER_ISOCHRONOUS &&
			    bytes >= payload && bytes < capacity) {
				selected = a;
				endpoint = ep;
				capacity = bytes;
			}
		}
	if (!selected || !payload || payload > USB_ISO_BUFFER_MAX)
		return -ENOSPC;
	ret = usb_control(usb, USB_TYPE_CLASS | USB_RECIP_INTERFACE, 1, 0x200,
			  interface->number, probe, length);
	if (ret < 0)
		return ret;
	ret = usb_set_interface(interface, selected->number);
	if (ret < 0)
		return ret;
	printk("usb-video: stream format=%08x size=%ux%u interval=%u payload=%u capacity=%u alternate=%u\n",
	       state->format.format, state->format.width, state->format.height,
	       le32(probe + 4), payload, capacity, selected->number);
	state->endpoint = endpoint;
	state->packet_size = capacity;
	state->packet_count = USB_ISO_BUFFER_MAX / capacity;
	if (state->packet_count > USB_ISO_PACKETS)
		state->packet_count = USB_ISO_PACKETS;
	return 0;
}
static void capture_complete(void *context, int result)
{
	struct capture_batch *batch = context;
	batch->result = result;
	__atomic_store_n(&batch->complete, 1, __ATOMIC_RELEASE);
	cond_notify(&batch->file->wake);
}
static int submit_batch(struct capture_file *state, unsigned bank)
{
	struct capture_batch *batch = &state->batches[bank];
	for (unsigned i = 0; i < state->packet_count; i++) {
		batch->packets[i].offset = i * state->packet_size;
		batch->packets[i].length = state->packet_size;
	}
	__atomic_store_n(&batch->complete, 0, __ATOMIC_RELEASE);
	int ret = usb_iso_submit(state->camera->interface->usb, state->endpoint,
				 batch->data, batch->packets,
				 state->packet_count, capture_complete, batch);
	batch->submitted = ret >= 0;
	return ret;
}
static void finish_frame(struct capture_file *state)
{
	if (state->filling < 0)
		return;
	struct capture_frame *frame = &state->frames[state->filling];
	int valid = !state->bad && state->accumulated &&
		    (state->format.format != UVC_YUYV ||
		     state->accumulated == state->format.size);
	frame->state = valid ? 2 : 1;
	frame->bytes = state->accumulated;
	frame->sequence = state->sequence++;
	frame->time = time_now_us();
	state->filling = -1;
	if (valid)
		video_notify(state);
}
static void consume_payload(struct capture_file *state,
			    const unsigned char *data, unsigned length)
{
	if (length < 2 || data[0] < 2 || data[0] > length)
		return;
	unsigned fid = data[1] & 1, eof = data[1] & 2;
	mutex_lock(&state->lock);
	if (!state->synchronized) {
		if (state->fid < 2 && state->fid != fid)
			state->synchronized = 1;
		else {
			state->fid = fid;
			if (eof)
				state->synchronized = 1;
			mutex_unlock(&state->lock);
			return;
		}
	}
	if (fid != state->fid) {
		finish_frame(state);
		state->accumulated = state->bad = 0;
		state->fid = fid;
	}
	unsigned bytes = length - data[0];
	if (state->filling < 0 && bytes) {
		for (unsigned i = 0; i < state->count; i++)
			if (state->frames[i].state == 1) {
				state->filling = i;
				state->frames[i].state = 3;
				state->accumulated = state->bad = 0;
				break;
			}
	}
	if (data[1] & 0x40)
		state->bad = 1;
	if (state->filling >= 0) {
		struct capture_frame *frame = &state->frames[state->filling];
		if (bytes > state->format.size - state->accumulated)
			state->bad = 1;
		else if (!state->bad) {
			memcpy((char *)frame->data + state->accumulated,
			       data + data[0], bytes);
			state->accumulated += bytes;
		}
		if (eof)
			finish_frame(state);
	}
	mutex_unlock(&state->lock);
}
static void capture_worker(void *context)
{
	struct capture_file *state = context;
	int error = 0;
	for (unsigned bank = 0; bank < 2 && !error; bank++)
		error = submit_batch(state, bank);
	while (!state->stopping && state->camera->online && !error) {
		cond_reset(&state->wake);
		for (unsigned bank = 0; bank < 2 && !error; bank++) {
			struct capture_batch *batch = &state->batches[bank];
			if (!__atomic_load_n(&batch->complete,
					     __ATOMIC_ACQUIRE))
				continue;
			batch->submitted = 0;
			if (batch->result < 0) {
				error = batch->result;
				break;
			}
			for (unsigned i = 0; i < state->packet_count; i++) {
				struct usb_iso_packet *packet =
					&batch->packets[i];
				if (!packet->status)
					consume_payload(
						state,
						(unsigned char *)batch->data +
							packet->offset,
						packet->actual);
				else {
					mutex_lock(&state->lock);
					state->bad = 1;
					mutex_unlock(&state->lock);
				}
			}
			if (!state->stopping)
				error = submit_batch(state, bank);
		}
		if (!error && !state->stopping)
			cond_wait(&state->wake, 0);
	}
	for (;;) {
		cond_reset(&state->wake);
		unsigned pending = 0;
		for (unsigned bank = 0; bank < 2; bank++)
			if (state->batches[bank].submitted &&
			    !__atomic_load_n(&state->batches[bank].complete,
					     __ATOMIC_ACQUIRE))
				pending++;
		if (!pending)
			break;
		cond_wait(&state->wake, 0);
	}
	if (state->camera->online)
		usb_set_interface(state->camera->interface, 0);
	mutex_lock(&state->lock);
	state->streaming = 0;
	state->error = error;
	state->worker = 0;
	video_notify(state);
	cond_notify(&state->stopped);
	mutex_unlock(&state->lock);
}
static int start_capture(struct capture_file *state)
{
	if (state->streaming)
		return 0;
	if (!state->camera->online || !state->count)
		return -ENODEV;
	int ret = probe_stream(state);
	if (ret < 0)
		return ret;
	for (unsigned bank = 0; bank < 2; bank++) {
		struct capture_batch *batch = &state->batches[bank];
		batch->file = state;
		if (!batch->data)
			batch->data = malloc(USB_ISO_BUFFER_MAX);
		if (!batch->data) {
			usb_set_interface(state->camera->interface, 0);
			return -ENOMEM;
		}
		batch->submitted = batch->complete = 0;
	}
	state->stopping = state->error = state->synchronized = 0;
	state->filling = -1;
	state->fid = 2;
	state->sequence = 0;
	state->streaming = state->worker = 1;
	cond_reset(&state->stopped);
	ps_create(capture_worker, state, ps_normal, ps_kernel);
	return 0;
}
static void stop_capture(struct capture_file *state)
{
	mutex_lock(&state->lock);
	unsigned running = state->worker;
	state->stopping = 1;
	mutex_unlock(&state->lock);
	if (!running)
		return;
	cond_notify(&state->wake);
	cond_wait(&state->stopped, 0);
	mutex_lock(&state->lock);
	mutex_unlock(&state->lock);
}
static int find_ready(struct capture_file *state)
{
	int ready = -1;
	for (unsigned i = 0; i < state->count; i++)
		if (state->frames[i].state == 2 &&
		    (ready < 0 || (int)(state->frames[i].sequence -
					state->frames[ready].sequence) < 0))
			ready = i;
	return ready;
}
static int wait_frame(struct capture_file *state, unsigned nonblocking)
{
	uint64_t deadline = time_deadline_ms(10000);
	for (;;) {
		mutex_lock(&state->lock);
		int index = find_ready(state);
		mutex_unlock(&state->lock);
		if (index >= 0)
			return index;
		if (!state->camera->online)
			return -ENODEV;
		if (state->error || !state->streaming)
			return state->error ? state->error : -EPIPE;
		if (nonblocking)
			return -EAGAIN;
		if (time_now_ms() >= deadline)
			return -ETIMEDOUT;
		if (ps_interrupting_signals(current))
			return -EINTR;
		time_wait(1);
	}
}
static void describe_buffer(struct capture_file *state,
			    struct video_buffer *buffer, unsigned index)
{
	struct capture_frame *frame = &state->frames[index];
	memset(buffer, 0, sizeof(*buffer));
	buffer->index = index;
	buffer->type = 1;
	buffer->memory = 1;
	buffer->location.offset = index * state->stride;
	buffer->length = state->stride;
	buffer->bytes = frame->bytes;
	buffer->field = 1;
	buffer->sequence = frame->sequence;
	buffer->seconds = frame->time / 1000000;
	buffer->microseconds = frame->time % 1000000;
	buffer->flags = 0x2000 | (frame->state == 2 ? 4 : frame->state ? 2 : 0);
}
static int video_ioctl(file *fp, unsigned command, void *data)
{
	struct capture_file *state = fp->f_inode->i_private;
	if (!state->camera->online)
		return -ENODEV;
	if (!data || ((command >> 8) & 255) != 'V')
		return -ENOTTY;
	unsigned nr = command & 255;
	static const unsigned sizes[76] = {
		[0] = sizeof(struct video_capability),
		[2] = sizeof(struct video_format_desc),
		[4] = sizeof(struct video_format),
		[5] = sizeof(struct video_format),
		[8] = sizeof(struct video_request_buffers),
		[9] = sizeof(struct video_buffer),
		[15] = sizeof(struct video_buffer),
		[17] = sizeof(struct video_buffer),
		[18] = 4,
		[19] = 4,
		[21] = sizeof(struct video_stream_parameters),
		[22] = sizeof(struct video_stream_parameters),
		[23] = 8,
		[24] = 8,
		[26] = sizeof(struct video_input),
		[38] = 4,
		[39] = 4,
		[64] = sizeof(struct video_format),
		[74] = sizeof(struct video_frame_size),
		[75] = sizeof(struct video_frame_interval),
	};
	if (nr >= 76 || !sizes[nr])
		return -EINVAL;
	if (((command >> 16) & 0x3fff) != sizes[nr] ||
	    !video_user_range(data, sizes[nr]))
		return -EFAULT;
	switch (nr) {
	case 0: {
		struct video_capability *cap = data;
		memset(cap, 0, sizeof(*cap));
		strcpy(cap->driver, "mos-uvc");
		strcpy(cap->card, "USB Video Camera");
		sprintf(cap->bus, "usb-%u-%s",
			state->camera->interface->usb->busnum,
			state->camera->interface->usb->path);
		cap->version = 0x020414;
		cap->device_caps = UVC_CAPTURE_CAPS;
		cap->capabilities = UVC_CAPTURE_CAPS | (1u << 31);
		return 0;
	}
	case 2: {
		struct video_format_desc *desc = data;
		if (desc->type != 1)
			return -EINVAL;
		unsigned formats[2] = { 0 }, count = 0;
		for (unsigned i = 0; i < state->camera->format_count; i++) {
			unsigned fmt = state->camera->formats[i].format;
			if ((!count || formats[0] != fmt) && count < 2)
				formats[count++] = fmt;
		}
		if (desc->index >= count)
			return -EINVAL;
		unsigned index = desc->index;
		memset(desc, 0, sizeof(*desc));
		desc->index = index;
		desc->type = 1;
		desc->format = formats[index];
		desc->flags = desc->format == UVC_MJPEG ? 1 : 0;
		strcpy(desc->description, desc->format == UVC_MJPEG ?
						  "Motion JPEG" :
						  "YUYV 4:2:2");
		return 0;
	}
	case 4:
	case 5:
	case 64: {
		struct video_format *format = data;
		if (format->type != 1)
			return -EINVAL;
		struct capture_format chosen = state->format;
		if (nr != 4) {
			unsigned distance = ~0u;
			for (unsigned i = 0; i < state->camera->format_count;
			     i++) {
				struct capture_format candidate =
					state->camera->formats[i];
				if (candidate.format != format->data.pix.format)
					continue;
				unsigned d =
					(candidate.width >
							 format->data.pix.width ?
						 candidate.width -
							 format->data.pix.width :
						 format->data.pix.width -
							 candidate.width) +
					(candidate.height > format->data.pix
								    .height ?
						 candidate.height -
							 format->data.pix.height :
						 format->data.pix.height -
							 candidate.height);
				if (d < distance) {
					chosen = candidate;
					distance = d;
				}
			}
			if (nr == 5) {
				if (state->worker || state->count)
					return -EBUSY;
				state->format = chosen;
			}
		}
		memset(&format->data, 0, sizeof(format->data));
		format->data.pix = (struct video_pix_format){
			.width = chosen.width,
			.height = chosen.height,
			.format = chosen.format,
			.field = 1,
			.stride = chosen.format == UVC_YUYV ? chosen.width * 2 :
							      0,
			.size = chosen.size,
			.colorspace = 8
		};
		return 0;
	}
	case 8: {
		struct video_request_buffers *request = data;
		if (request->type != 1 || request->memory != 1)
			return -EINVAL;
		if (state->worker || fp->f_count > 2)
			return -EBUSY;
		unsigned count = request->count > UVC_BUFFERS ? UVC_BUFFERS :
								request->count;
		int ret = allocate_frames(state, count);
		request->count = state->count;
		request->capabilities = 1;
		return ret;
	}
	case 9:
	case 15: {
		struct video_buffer *buffer = data;
		if (buffer->type != 1 || buffer->memory != 1 ||
		    buffer->index >= state->count)
			return -EINVAL;
		mutex_lock(&state->lock);
		unsigned index = buffer->index;
		if (nr == 15) {
			if (state->frames[index].state) {
				mutex_unlock(&state->lock);
				return -EBUSY;
			}
			state->frames[index].state = 1;
		}
		describe_buffer(state, buffer, index);
		mutex_unlock(&state->lock);
		return 0;
	}
	case 17: {
		struct video_buffer *buffer = data;
		if (buffer->type != 1 || buffer->memory != 1)
			return -EINVAL;
		int index = wait_frame(state, fp->f_flag & O_NONBLOCK);
		if (index < 0)
			return index;
		mutex_lock(&state->lock);
		describe_buffer(state, buffer, index);
		state->frames[index].state = 0;
		mutex_unlock(&state->lock);
		return 0;
	}
	case 18:
		if (*(unsigned *)data != 1)
			return -EINVAL;
		return start_capture(state);
	case 19:
		if (*(unsigned *)data != 1)
			return -EINVAL;
		stop_capture(state);
		for (unsigned i = 0; i < state->count; i++)
			state->frames[i].state = 0;
		return 0;
	case 21:
	case 22: {
		struct video_stream_parameters *p = data;
		if (p->type != 1)
			return -EINVAL;
		memset(&p->data, 0, sizeof(p->data));
		p->data.capture.capabilities = 0x1000;
		p->data.capture.numerator = state->format.interval;
		p->data.capture.denominator = 10000000;
		p->data.capture.buffers = state->count;
		return 0;
	}
	case 23:
		*(uint64_t *)data = 0;
		return 0;
	case 24:
		return 0;
	case 26: {
		struct video_input *input = data;
		if (input->index)
			return -EINVAL;
		memset(input, 0, sizeof(*input));
		strcpy(input->name, "USB Camera");
		input->type = 2;
		return 0;
	}
	case 38:
		*(unsigned *)data = 0;
		return 0;
	case 39:
		return *(unsigned *)data ? -EINVAL : 0;
	case 74: {
		struct video_frame_size *f = data;
		unsigned index = 0;
		for (unsigned i = 0; i < state->camera->format_count; i++) {
			struct capture_format *c = &state->camera->formats[i];
			if (c->format == f->format && index++ == f->index) {
				f->type = 1;
				f->size.discrete.width = c->width;
				f->size.discrete.height = c->height;
				return 0;
			}
		}
		return -EINVAL;
	}
	case 75: {
		struct video_frame_interval *f = data;
		if (f->index)
			return -EINVAL;
		for (unsigned i = 0; i < state->camera->format_count; i++) {
			struct capture_format *c = &state->camera->formats[i];
			if (c->format == f->format && c->width == f->width &&
			    c->height == f->height) {
				f->type = 1;
				f->interval.discrete.numerator = c->interval;
				f->interval.discrete.denominator = 10000000;
				return 0;
			}
		}
		return -EINVAL;
	}
	default:
		return -EINVAL;
	}
}
static ssize_t video_read(file *fp, void *data, size_t length, loff_t *position)
{
	(void)position;
	struct capture_file *state = fp->f_inode->i_private;
	if (!length)
		return 0;
	if (length < state->format.size)
		return -EINVAL;
	if (!video_user_range(data, state->format.size))
		return -EFAULT;
	if (!state->count) {
		int ret = allocate_frames(state, UVC_BUFFERS);
		if (ret < 0)
			return ret;
		for (unsigned i = 0; i < state->count; i++)
			state->frames[i].state = 1;
	}
	int ret = start_capture(state);
	if (ret < 0)
		return ret;
	int index = wait_frame(state, fp->f_flag & O_NONBLOCK);
	if (index < 0)
		return index;
	mutex_lock(&state->lock);
	unsigned bytes = state->frames[index].bytes;
	memcpy(data, state->frames[index].data, bytes);
	state->frames[index].state = 1;
	mutex_unlock(&state->lock);
	return bytes;
}
static unsigned video_poll(file *fp, unsigned events, poll_table *table)
{
	struct capture_file *state = fp->f_inode->i_private;
	mutex_lock(&state->lock);
	if (table)
		poll_subscribe(table, &state->waiters, &state->wait_lock);
	unsigned ready = !state->camera->online || state->error ? FS_POLL_ERR :
			 find_ready(state) >= 0			? FS_POLL_READ :
								  0;
	mutex_unlock(&state->lock);
	return events & ready;
}
static int video_map(file *fp, uint64_t *offset, size_t length, unsigned prot,
		     unsigned flags, file **backing)
{
	struct capture_file *state = fp->f_inode->i_private;
	if (!state->count || !state->stride || !length ||
	    *offset % state->stride ||
	    *offset / state->stride >= state->count || length > state->stride ||
	    (flags & MAP_TYPE) != MAP_SHARED || (prot & PROT_EXEC))
		return -EINVAL;
	fs_get_file(fp);
	*backing = fp;
	return 0;
}
static paddr_t video_page(file *fp, uint64_t offset)
{
	struct capture_file *state = fp->f_inode->i_private;
	if (!state->stride || offset / state->stride >= state->count ||
	    (offset & (PAGE_SIZE - 1)))
		return 0;
	return VIRT_TO_PHY((char *)state->frames[offset / state->stride].data +
			   offset % state->stride);
}
static int video_stat(file *fp, struct stat *stat)
{
	struct capture_file *state = fp->f_inode->i_private;
	memset(stat, 0, sizeof(*stat));
	stat->st_mode = S_IFCHR | 0666;
	stat->st_rdev = MKDEV(USB_VIDEO_MAJOR, state->camera->index);
	stat->st_nlink = 1;
	stat->st_blksize = PAGE_SIZE;
	return 0;
}
static int video_close(file *fp)
{
	struct capture_file *state = fp->f_inode->i_private;
	stop_capture(state);
	mutex_lock(&state->camera->lock);
	if (state->camera->owner == state)
		state->camera->owner = NULL;
	mutex_unlock(&state->camera->lock);
	allocate_frames(state, 0);
	for (unsigned i = 0; i < 2; i++)
		free(state->batches[i].data);
	ref_count_put(state->camera);
	free(state);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static const file_operations video_operations = {
	.read = video_read,
	.ioctl = video_ioctl,
	.poll = video_poll,
	.getattr = video_stat,
	.release = video_close,
	.mmap_file = video_map,
	.map_page = video_page,
};
static file *video_open(super_block *sb, unsigned rdev, int flags)
{
	(void)sb;
	unsigned index = MINOR(rdev);
	if (index >= UVC_CAMERAS)
		return NULL;
	int irq;
	spinlock_lock(&cameras_lock, &irq);
	struct camera *camera = cameras[index];
	if (camera && camera->online)
		ref_count_get(camera);
	else
		camera = NULL;
	spinlock_unlock(&cameras_lock, irq);
	if (!camera)
		return NULL;
	mutex_lock(&camera->lock);
	if (camera->owner) {
		mutex_unlock(&camera->lock);
		ref_count_put(camera);
		return NULL;
	}
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	struct capture_file *state = zalloc(sizeof(*state));
	if (!fp || !node || !state) {
		free(fp);
		free(node);
		free(state);
		mutex_unlock(&camera->lock);
		ref_count_put(camera);
		return NULL;
	}
	state->camera = camera;
	state->format = camera->formats[0];
	for (unsigned i = 0; i < camera->format_count; i++)
		if (camera->formats[i].width == 320 &&
		    camera->formats[i].height == 240 &&
		    camera->formats[i].format == UVC_YUYV)
			state->format = camera->formats[i];
	mutex_init(&state->lock);
	cond_init(&state->wake, 1);
	cond_init(&state->stopped, 1);
	list_init(&state->waiters);
	spinlock_init(&state->wait_lock);
	camera->owner = state;
	mutex_unlock(&camera->lock);
	node->i_private = state;
	node->i_mode = S_IFCHR | 0666;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_flag = flags;
	fp->f_mode = flags & O_ACCMODE;
	fp->f_fop = &video_operations;
	return fp;
}
static void video_disconnect(struct usb_interface *interface)
{
	struct camera *camera = interface->class_data;
	if (!camera)
		return;
	int irq;
	spinlock_lock(&cameras_lock, &irq);
	camera->online = 0;
	cameras[camera->index] = NULL;
	spinlock_unlock(&cameras_lock, irq);
	mutex_lock(&camera->lock);
	if (camera->owner) {
		cond_notify(&camera->owner->wake);
		video_notify(camera->owner);
	}
	mutex_unlock(&camera->lock);
	char name[32], path[256];
	sprintf(name, "video%u", camera->index);
	sprintf(path, "%s/%s/video4linux/%s", interface->usb->sysfs_path,
		vfs_entry_name(interface->device.sysfs_entry), name);
	netlink_device_event("remove", path, "video4linux", name,
			     USB_VIDEO_MAJOR, camera->index);
	sprintf(name, "/video%u", camera->index);
	vfs_entry_remove(devfs_entries(), name);
	sprintf(name, "video%u", camera->index);
	vfs_entry_node *root = vfs_entry_root(device_sysfs_entries());
	vfs_entry_remove(vfs_entry_child(vfs_entry_child(root, "class"),
					 "video4linux"),
			 name);
	interface->class_data = NULL;
	ref_count_put(camera);
}
static int video_probe(struct usb_interface *interface)
{
	struct camera *camera = zalloc(sizeof(*camera));
	if (!camera)
		return -ENOMEM;
	ref_count_init(&camera->references, camera_release);
	camera->interface = interface;
	ref_count_get(interface->usb);
	mutex_init(&camera->lock);
	int ret = parse_formats(camera);
	if (ret < 0) {
		ref_count_put(camera);
		return ret;
	}
	int irq;
	spinlock_lock(&cameras_lock, &irq);
	unsigned index;
	for (index = 0; index < UVC_CAMERAS && cameras[index]; index++)
		;
	if (index == UVC_CAMERAS) {
		spinlock_unlock(&cameras_lock, irq);
		ref_count_put(camera);
		return -ENOSPC;
	}
	camera->index = index;
	camera->online = 1;
	cameras[index] = camera;
	spinlock_unlock(&cameras_lock, irq);
	interface->class_data = camera;
	char name[40], text[32];
	sprintf(name, "/video%u", index);
	vfs_entry_device(devfs_entries(), name, S_IFCHR | 0666,
			 MKDEV(USB_VIDEO_MAJOR, index), "video4linux",
			 video_open);
	vfs_entry_node *root = vfs_entry_root(device_sysfs_entries());
	vfs_entry_node *class_directory = vfs_entry_directory(
		vfs_entry_directory(root, "class"), "video4linux");
	vfs_entry_node *directory = vfs_entry_directory(
		interface->device.sysfs_entry, "video4linux");
	sprintf(name, "video%u", index);
	directory = vfs_entry_directory(directory, name);
	vfs_entry_link(class_directory, name, directory);
	vfs_entry_link(directory, "subsystem", class_directory);
	vfs_entry_text(directory, "name", "USB Video Camera\n");
	sprintf(text, "%u\n", index);
	vfs_entry_text(directory, "index", text);
	sprintf(text, "%u:%u\n", USB_VIDEO_MAJOR, index);
	vfs_entry_text(directory, "dev", text);
	char event[128];
	sprintf(event, "MAJOR=%u\nMINOR=%u\nDEVNAME=video%u\n", USB_VIDEO_MAJOR,
		index, index);
	vfs_entry_text(directory, "uevent", event);
	vfs_entry_link(directory, "device", interface->device.sysfs_entry);
	char path[256];
	sprintf(path, "%s/%s/video4linux/video%u", interface->usb->sysfs_path,
		vfs_entry_name(interface->device.sysfs_entry), index);
	vfs_entry_text(directory, "devpath", path);
	netlink_device_event("add", path, "video4linux", name, USB_VIDEO_MAJOR,
			     index);
	printk("usb-video: /dev/video%u formats=%u interface=%u\n", index,
	       camera->format_count, interface->number);
	return 0;
}
static driver_t video_driver = {
	.name = "usb-video",
	.bus = DEVICE_BUS_USB,
	.usb_class = 14,
	.usb_subclass = 2,
	.probe_usb = video_probe,
	.disconnect_usb = video_disconnect,
};
DRIVER_REGISTER(video_driver);
