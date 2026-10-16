/* USB Audio Class 1 Type-I PCM playback through the OSS device interface. */
#include <device/usb/usb.h>
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
struct usb_audio {
	ref_count_t references;
	struct usb_interface *interface;
	struct usb_endpoint *out;
	mutex_t lock;
	cond_t completion;
	unsigned char buffer[UAC_BYTES];
	struct usb_iso_packet packets[USB_ISO_PACKETS];
	unsigned volume, bytes, blocks, ready;
	int result;
};
static struct usb_audio *output;
static spinlock_t output_lock = SPINLOCK_INITIALIZER;
static void audio_release(ref_count_t *ref)
{
	struct usb_audio *audio = (void *)ref;
	ref_count_put(audio->interface->usb);
	free(audio);
}
static void audio_complete(void *context, int result)
{
	struct usb_audio *audio = context;
	audio->result = result;
	cond_notify(&audio->completion);
}
static ssize_t audio_write(file *fp, const void *buffer, size_t length,
			   loff_t *position)
{
	(void)position;
	struct usb_audio *audio = fp->f_inode->i_private;
	if (!length)
		return 0;
	if (!buffer || length % UAC_FRAME_BYTES)
		return -EINVAL;
	mutex_lock(&audio->lock);
	unsigned done = 0;
	int ret = 0;
	while (done < length && audio->ready && audio->interface->usb->online) {
		unsigned bytes = length - done;
		if (bytes > UAC_BYTES)
			bytes = UAC_BYTES;
		memcpy(audio->buffer, (const char *)buffer + done, bytes);
		unsigned left = audio->volume & 255,
			 right = (audio->volume >> 8) & 255;
		int16_t *samples = (void *)audio->buffer;
		for (unsigned i = 0; i < bytes / 2; i++)
			samples[i] =
				samples[i] * (int)(i & 1 ? right : left) / 100;
		unsigned count =
			(bytes + UAC_PACKET_BYTES - 1) / UAC_PACKET_BYTES;
		for (unsigned i = 0; i < count; i++) {
			unsigned size = bytes - i * UAC_PACKET_BYTES;
			audio->packets[i].offset = i * UAC_PACKET_BYTES;
			audio->packets[i].length = size > UAC_PACKET_BYTES ?
							   UAC_PACKET_BYTES :
							   size;
		}
		cond_reset(&audio->completion);
		ret = usb_iso_submit(audio->interface->usb, audio->out,
				     audio->buffer, audio->packets, count,
				     audio_complete, audio);
		if (ret < 0)
			break;
		int interrupted = cond_wait(&audio->completion, 1) < 0;
		if (interrupted)
			cond_wait(&audio->completion, 0);
		ret = audio->result;
		for (unsigned i = 0; i < count && !ret; i++)
			ret = audio->packets[i].status;
		if (interrupted)
			ret = -EINTR;
		if (ret < 0)
			break;
		done += bytes;
		audio->bytes += bytes;
		audio->blocks++;
	}
	if (!audio->ready || !audio->interface->usb->online)
		ret = -ENODEV;
	mutex_unlock(&audio->lock);
	return done ? (ssize_t)done : ret;
}
static unsigned audio_poll(file *fp, unsigned events, poll_table *table)
{
	(void)table;
	struct usb_audio *audio = fp->f_inode->i_private;
	return events &
	       (audio->ready && audio->interface->usb->online ? FS_POLL_WRITE :
								FS_POLL_ERR);
}
static int audio_ioctl(file *fp, unsigned command, void *buffer)
{
	struct usb_audio *audio = fp->f_inode->i_private;
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
		audio->bytes = audio->blocks = 0;
		mutex_unlock(&audio->lock);
		return 0;
	}
	if (nr == 1 || nr == 8 || nr == 10 || nr == 14 || nr == 16)
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
		*(int *)buffer = 4096;
		return 0;
	case 5:
		*(int *)buffer = UAC_FORMAT_S16_LE;
		return 0;
	case 6:
		*(int *)buffer = 2;
		return 0;
	case 9:
		return 0; /* Fragment size is fixed at 4096 bytes. */
	case 11:
		*(int *)buffer = UAC_FORMAT_S16_LE;
		return 0;
	case 12: {
		int *info = buffer;
		info[0] = info[1] = 32;
		info[2] = 4096;
		info[3] = 32 * 4096;
		return 0;
	}
	case 15:
		*(int *)buffer = 0x1000;
		return 0;
	case 17: {
		unsigned *info = buffer;
		info[0] = audio->bytes;
		info[1] = audio->blocks;
		info[2] = 0;
		return 0;
	}
	case 18:
		*(int *)buffer = 2;
		return 0;
	case 23:
		*(int *)buffer = 0;
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
	ref_count_put(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
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
	if (!fp || !node) {
		free(fp);
		free(node);
		ref_count_put(audio);
		return NULL;
	}
	node->i_mode = S_IFCHR | 0666;
	node->i_private = audio;
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
	vfs_entry_remove(devfs_entries(), "/dsp");
	vfs_entry_remove(devfs_entries(), "/audio");
	vfs_entry_remove(devfs_entries(), "/mixer");
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
	cond_init(&audio->completion, 1);
	audio->out = ep;
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
