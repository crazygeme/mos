/* USB Audio Class 1 Type-I PCM playback through the OSS device interface. */
#include <device/usb/usb.h>
#include <driver/audio/alsa.h>
#include <device/devnums.h>
#include <device/devnode.h>
#include <fs/entries.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <errno.h>
#include <macro.h>

#define UAC_RATE 48000
#define UAC_FRAME_BYTES 4
#define UAC_PACKET_BYTES 192
#define UAC_FORMAT_S16_LE 0x10
#define UAC_BYTES (USB_ISO_PACKETS * UAC_PACKET_BYTES)
struct usb_audio;
struct audio_stream;
struct audio_transfer {
	unsigned char buffer[UAC_BYTES];
	struct usb_iso_packet packets[USB_ISO_PACKETS];
	cond_t completion;
	struct audio_stream *stream;
	unsigned bytes, count, pending, done, accounted, packets_accounted;
	int result;
};
struct usb_audio {
	ref_count_t references;
	struct usb_interface *interface;
	struct usb_endpoint *out;
	mutex_t lock;
	struct audio_transfer transfers[2];
	unsigned volume, ready, next;
};
struct audio_stream {
	struct usb_audio *audio;
	uint64_t bytes, reported_fragments;
	unsigned fragment_bytes, fragments, queued, partial_bytes;
	unsigned char partial[UAC_PACKET_BYTES];
	int error;
};
static struct usb_audio *output;
static spinlock_t output_lock = SPINLOCK_INITIALIZER;
static void audio_release(ref_count_t *ref)
{
	struct usb_audio *audio = (void *)ref;
	ref_count_put(audio->interface->usb);
	free(audio);
}
static void audio_progress(struct usb_endpoint *endpoint)
{
	(void)endpoint;
	alsa_wake();
}
static void audio_complete(void *context, int result)
{
	struct audio_transfer *transfer = context;
	transfer->result = result;
	__atomic_store_n(&transfer->done, 1, __ATOMIC_RELEASE);
	cond_notify(&transfer->completion);
}
/* The device lock serializes queue accounting; callbacks only publish completion. */
static void audio_reap(struct usb_audio *audio)
{
	for (unsigned bank = 0; bank < 2; bank++) {
		struct audio_transfer *t = &audio->transfers[bank];
		if (!t->pending)
			continue;
		while (t->packets_accounted < t->count) {
			struct usb_iso_packet *p =
				&t->packets[t->packets_accounted];
			int status =
				__atomic_load_n(&p->status, __ATOMIC_ACQUIRE);
			if (status == -EINPROGRESS)
				break;
			unsigned valid = t->bytes - t->accounted;
			if (valid > p->length)
				valid = p->length;
			if (status < 0)
				t->stream->error = status;
			else
				t->stream->bytes += valid;
			t->stream->queued -= valid;
			t->accounted += valid;
			t->packets_accounted++;
		}
		if (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE) ||
		    t->packets_accounted != t->count)
			continue;
		if (t->result < 0)
			t->stream->error = t->result;
		t->pending = 0;
		t->stream = NULL;
	}
}
static void audio_wait(struct usb_audio *audio, struct audio_transfer *t)
{
	while (t->pending && !__atomic_load_n(&t->done, __ATOMIC_ACQUIRE))
		cond_wait(&t->completion, 0);
	audio_reap(audio);
}
static int audio_submit(struct audio_stream *stream, struct audio_transfer *t,
			unsigned bytes, unsigned valid)
{
	struct usb_audio *audio = stream->audio;
	unsigned left = audio->volume & 255, right = (audio->volume >> 8) & 255;
	int16_t *samples = (void *)t->buffer;
	for (unsigned i = 0; i < bytes / 2; i++)
		samples[i] = samples[i] * (int)(i & 1 ? right : left) / 100;
	t->count = bytes / UAC_PACKET_BYTES;
	for (unsigned i = 0; i < t->count; i++) {
		t->packets[i].offset = i * UAC_PACKET_BYTES;
		t->packets[i].length = UAC_PACKET_BYTES;
	}
	cond_reset(&t->completion);
	__atomic_store_n(&t->done, 0, __ATOMIC_RELEASE);
	t->stream = stream;
	t->bytes = valid;
	t->accounted = t->packets_accounted = 0;
	int ret = usb_iso_submit(audio->interface->usb, audio->out, t->buffer,
				 t->packets, t->count, audio_complete, t);
	if (ret < 0) {
		t->stream = NULL;
		return ret;
	}
	t->pending = 1;
	audio->next ^= 1;
	return 0;
}
static int audio_drain(struct audio_stream *stream)
{
	struct usb_audio *audio = stream->audio;
	if (stream->partial_bytes && audio->ready &&
	    audio->interface->usb->online) {
		struct audio_transfer *t = &audio->transfers[audio->next];
		audio_wait(audio, t);
		memcpy(t->buffer, stream->partial, stream->partial_bytes);
		memset(t->buffer + stream->partial_bytes, 0,
		       UAC_PACKET_BYTES - stream->partial_bytes);
		int ret = audio_submit(stream, t, UAC_PACKET_BYTES,
				       stream->partial_bytes);
		if (ret < 0) {
			stream->error = ret;
			stream->queued -= stream->partial_bytes;
		}
		stream->partial_bytes = 0;
	}
	for (unsigned bank = 0; bank < 2; bank++) {
		struct audio_transfer *t = &audio->transfers[bank];
		if (t->pending && t->stream == stream)
			audio_wait(audio, t);
	}
	return stream->error;
}
static ssize_t audio_write(file *fp, const void *buffer, size_t length,
			   loff_t *position)
{
	(void)position;
	struct audio_stream *stream = fp->f_inode->i_private;
	struct usb_audio *audio = stream->audio;
	if (!length)
		return 0;
	if (!buffer || length % UAC_FRAME_BYTES)
		return -EINVAL;
	mutex_lock(&audio->lock);
	size_t done = 0;
	int ret = 0;
	while (done < length && audio->ready && audio->interface->usb->online) {
		audio_reap(audio);
		if (stream->error) {
			ret = stream->error;
			break;
		}
		struct audio_transfer *t = &audio->transfers[audio->next];
		unsigned capacity = stream->fragment_bytes * stream->fragments;
		if (t->pending || stream->queued >= capacity) {
			if (fp->f_flag & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}
			if (!t->pending)
				t = &audio->transfers[audio->next ^ 1];
			audio_wait(audio, t);
			continue;
		}
		unsigned bytes =
			length - done > UAC_BYTES - stream->partial_bytes ?
				UAC_BYTES - stream->partial_bytes :
				length - done;
		if (bytes > capacity - stream->queued)
			bytes = capacity - stream->queued;
		unsigned partial = stream->partial_bytes;
		memcpy(t->buffer, stream->partial, partial);
		memcpy(t->buffer + partial, (const char *)buffer + done, bytes);
		unsigned total = partial + bytes;
		unsigned payload = total / UAC_PACKET_BYTES * UAC_PACKET_BYTES;
		if (payload) {
			ret = audio_submit(stream, t, payload, payload);
			if (ret < 0)
				break;
		}
		stream->partial_bytes = total - payload;
		memcpy(stream->partial, t->buffer + payload,
		       stream->partial_bytes);
		stream->queued += bytes;
		done += bytes;
	}
	if (!audio->ready || !audio->interface->usb->online)
		ret = -ENODEV;
	mutex_unlock(&audio->lock);
	return done ? (ssize_t)done : ret;
}
static unsigned audio_poll(file *fp, unsigned events, poll_table *table)
{
	(void)table;
	struct audio_stream *stream = fp->f_inode->i_private;
	struct usb_audio *audio = stream->audio;
	return events &
	       (audio->ready && audio->interface->usb->online ? FS_POLL_WRITE :
								FS_POLL_ERR);
}
static int audio_ioctl(file *fp, unsigned command, void *buffer)
{
	struct audio_stream *stream = fp->f_inode->i_private;
	struct usb_audio *audio = stream->audio;
	if (!audio->ready || !audio->interface->usb->online)
		return -ENODEV;
	unsigned group = (command >> 8) & 255, nr = command & 255;
	if (group == 'M') {
		if (!buffer)
			return -EFAULT;
		unsigned mask = (1u << 0) | (1u << 4);
		if (nr == 118) {
			*(int *)buffer = 0x030802;
			return 0;
		}
		if (nr == 101) {
			struct {
				char id[16], name[32];
				int count, filler[10];
			} info = { 0 };
			strcpy(info.id, "MOSUSBAUDIO");
			strcpy(info.name, "MOS USB Audio Mixer");
			unsigned size = (command >> 16) & 0x3fff;
			memcpy(buffer, &info,
			       size < sizeof(info) ? size : sizeof(info));
			return 0;
		}
		if (nr == 254 || nr == 251) {
			*(int *)buffer = mask;
			return 0;
		}
		if (nr == 255 || nr == 253 || nr == 252) {
			*(int *)buffer = 0;
			return 0;
		}
		if (nr != 0 && nr != 4)
			return -EINVAL;
		mutex_lock(&audio->lock);
		if (command & (1u << 30)) {
			unsigned v = *(unsigned *)buffer;
			unsigned l = v & 255, r = (v >> 8) & 255;
			audio->volume = (l > 100 ? 100 : l) |
					((r > 100 ? 100 : r) << 8);
		}
		*(unsigned *)buffer = audio->volume;
		mutex_unlock(&audio->lock);
		return 0;
	}
	if (group != 'P')
		return -ENOTTY;
	if (nr == 0) {
		mutex_lock(&audio->lock);
		audio_drain(stream);
		stream->bytes = stream->reported_fragments = 0;
		stream->error = 0;
		mutex_unlock(&audio->lock);
		return 0;
	}
	if (nr == 1 || nr == 8) {
		mutex_lock(&audio->lock);
		int ret = audio_drain(stream);
		mutex_unlock(&audio->lock);
		return ret;
	}
	if (nr == 14)
		return 0;
	if (!buffer)
		return -EFAULT;
	switch (nr) {
	case 2:
		*(int *)buffer = UAC_RATE;
		return 0;
	case 3:
		*(int *)buffer = 1;
		return 0;
	case 4:
		*(int *)buffer = stream->fragment_bytes;
		return 0;
	case 5:
		*(int *)buffer = UAC_FORMAT_S16_LE;
		return 0;
	case 6:
		*(int *)buffer = 2;
		return 0;
	case 10: {
		unsigned config = *(unsigned *)buffer;
		unsigned shift = config & 0xffff, fragments = config >> 16;
		if (shift < 8)
			shift = 8;
		if (shift > 16)
			shift = 16;
		if (fragments < 2)
			fragments = 2;
		if (fragments > 1024)
			fragments = 1024;
		mutex_lock(&audio->lock);
		audio_drain(stream);
		stream->fragment_bytes = 1u << shift;
		stream->fragments = fragments;
		stream->bytes = stream->reported_fragments = 0;
		stream->error = 0;
		mutex_unlock(&audio->lock);
		*(unsigned *)buffer = shift | (fragments << 16);
		return 0;
	}
	case 11:
		*(int *)buffer = UAC_FORMAT_S16_LE;
		return 0;
	case 12: {
		int *info = buffer;
		mutex_lock(&audio->lock);
		audio_reap(audio);
		unsigned available =
			stream->fragments * stream->fragment_bytes -
			stream->queued;
		info[0] = available / stream->fragment_bytes;
		info[1] = stream->fragments;
		info[2] = stream->fragment_bytes;
		info[3] = available;
		mutex_unlock(&audio->lock);
		return 0;
	}
	case 15:
		*(int *)buffer = 0x1000;
		return 0;
	case 18: {
		unsigned *info = buffer;
		mutex_lock(&audio->lock);
		audio_reap(audio);
		uint64_t fragments = stream->bytes / stream->fragment_bytes;
		info[0] = stream->bytes;
		info[1] = fragments - stream->reported_fragments;
		info[2] = stream->bytes %
			  (stream->fragments * stream->fragment_bytes);
		stream->reported_fragments = fragments;
		mutex_unlock(&audio->lock);
		return 0;
	}
	case 16:
		*(int *)buffer = 2;
		return 0;
	case 23:
		mutex_lock(&audio->lock);
		audio_reap(audio);
		*(int *)buffer = stream->queued;
		mutex_unlock(&audio->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}
static int audio_getattr(file *fp, struct stat *stat)
{
	memset(stat, 0, sizeof(*stat));
	stat->st_mode = S_IFCHR | 0666;
	stat->st_rdev = MKDEV(SOUND_MAJOR, SOUND_DSP_MINOR);
	stat->st_blksize = 4096;
	stat->st_nlink = 1;
	(void)fp;
	return 0;
}
static int audio_close(file *fp)
{
	struct audio_stream *stream = fp->f_inode->i_private;
	mutex_lock(&stream->audio->lock);
	int ret = audio_drain(stream);
	mutex_unlock(&stream->audio->lock);
	ref_count_put(stream->audio);
	free(stream);
	free(fp->f_inode);
	free(fp);
	return ret;
}
static const file_operations audio_operations = {
	.write = audio_write,
	.ioctl = audio_ioctl,
	.poll = audio_poll,
	.getattr = audio_getattr,
	.release = audio_close,
};
static file *audio_open(super_block *sb, unsigned rdev, int flags)
{
	(void)sb;
	(void)rdev;
	int irq;
	spinlock_lock(&output_lock, &irq);
	struct usb_audio *audio = output;
	if (audio && audio->ready && audio->interface->usb->online)
		ref_count_get(audio);
	else
		audio = NULL;
	spinlock_unlock(&output_lock, irq);
	if (!audio)
		return NULL;
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	struct audio_stream *stream = zalloc(sizeof(*stream));
	if (!fp || !node || !stream) {
		free(fp);
		free(node);
		free(stream);
		ref_count_put(audio);
		return NULL;
	}
	node->i_mode = S_IFCHR | 0666;
	stream->audio = audio;
	stream->fragment_bytes = 4096;
	stream->fragments = 32;
	node->i_private = stream;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_mode = flags & O_ACCMODE;
	fp->f_flag = flags;
	fp->f_fop = &audio_operations;
	return fp;
}
static void audio_disconnect(struct usb_interface *interface)
{
	struct usb_audio *audio = interface->class_data;
	if (!audio)
		return;
	int irq;
	spinlock_lock(&output_lock, &irq);
	audio->ready = 0;
	if (output == audio)
		output = NULL;
	spinlock_unlock(&output_lock, irq);
	alsa_disconnect();
	vfs_entry_remove(devfs_entries(), "/dsp");
	vfs_entry_remove(devfs_entries(), "/audio");
	vfs_entry_remove(devfs_entries(), "/mixer");
	audio->out->tick = NULL;
	interface->class_data = NULL;
	ref_count_put(audio);
}
static int audio_probe(struct usb_interface *interface)
{
	if (interface->protocol)
		return -EOPNOTSUPP;
	struct usb_alternate *selected = NULL;
	for (struct usb_alternate *alt = interface->alternates; alt;
	     alt = alt->next) {
		for (unsigned offset = 0;
		     offset + 2 <= alt->descriptor_length;) {
			const unsigned char *d = alt->descriptors + offset;
			if (d[0] < 2 || d[0] > alt->descriptor_length - offset)
				return -EINVAL;
			if (d[1] == 0x24 && d[0] >= 11 && d[2] == 2 &&
			    d[3] == 1 && d[4] == 2 && d[5] == 2 && d[6] == 16 &&
			    d[7] && d[0] >= 8 + 3 * d[7]) {
				for (unsigned i = 0; i < d[7]; i++) {
					const unsigned char *r = d + 8 + 3 * i;
					if ((r[0] | (unsigned)r[1] << 8 |
					     (unsigned)r[2] << 16) == UAC_RATE)
						selected = alt;
				}
			}
			offset += d[0];
		}
	}
	if (!selected || selected->endpoint_count != 1)
		return -EOPNOTSUPP;
	struct usb_endpoint *ep = &selected->endpoints[0];
	if ((ep->address & USB_DIR_IN) ||
	    (ep->attributes & USB_ENDPOINT_TYPE_MASK) != USB_XFER_ISOCHRONOUS ||
	    (ep->attributes & 0x0c) == 4 || ep->max_packet < UAC_PACKET_BYTES ||
	    interface->usb->speed != USB_SPEED_FULL || ep->interval != 1)
		return -EOPNOTSUPP;
	int irq;
	spinlock_lock(&output_lock, &irq);
	int busy = output != NULL;
	spinlock_unlock(&output_lock, irq);
	if (busy)
		return -EBUSY;
	struct usb_audio *audio = zalloc(sizeof(*audio));
	if (!audio)
		return -ENOMEM;
	ref_count_init(&audio->references, audio_release);
	audio->interface = interface;
	ref_count_get(interface->usb);
	mutex_init(&audio->lock);
	for (unsigned bank = 0; bank < 2; bank++)
		cond_init(&audio->transfers[bank].completion, 1);
	audio->out = ep;
	ep->tick = audio_progress;
	audio->volume = 100 | (100 << 8);
	int ret = usb_set_interface(interface, selected->number);
	if (ret < 0) {
		ref_count_put(audio);
		return ret;
	}
	audio->ready = 1;
	interface->class_data = audio;
	spinlock_lock(&output_lock, &irq);
	output = audio;
	spinlock_unlock(&output_lock, irq);
	vfs_entry_device(devfs_entries(), "/dsp", S_IFCHR | 0666,
			 MKDEV(SOUND_MAJOR, SOUND_DSP_MINOR), "sound",
			 audio_open);
	vfs_entry_device(devfs_entries(), "/audio", S_IFCHR | 0666,
			 MKDEV(SOUND_MAJOR, SOUND_AUDIO_MINOR), "sound",
			 audio_open);
	vfs_entry_device(devfs_entries(), "/mixer", S_IFCHR | 0666,
			 MKDEV(SOUND_MAJOR, SOUND_MIXER_MINOR), "sound",
			 audio_open);
	alsa_register(audio_open);
	printk("usb-audio: playback 48000 Hz stereo S16_LE interface=%u alternate=%u\n",
	       interface->number, selected->number);
	return 0;
}
static driver_t audio_driver = {
	.name = "usb-audio",
	.bus = DEVICE_BUS_USB,
	.usb_class = USB_CLASS_AUDIO,
	.usb_subclass = 2,
	.probe_usb = audio_probe,
	.disconnect_usb = audio_disconnect,
};
DRIVER_REGISTER(audio_driver);
