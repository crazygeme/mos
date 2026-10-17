/* USB descriptor inventory and read-only usbfs device inspection. */
#include "usb.h"
#include <fs/sysfs.h>
#include <fs/fcntl.h>
#include <device/devnums.h>
#include <device/devnode.h>
#include <ps/ps.h>
#include <mm/mmu.h>
#include <lib/auto_free.h>
#include <errno.h>

#define USBFS_BUSES 32
#define USBFS_ADDRESSES 128
#define USBFS_MAJOR 189
struct usb_blob {
	unsigned length;
	unsigned char bytes[];
};
static struct usb_device *devices[USBFS_BUSES][USBFS_ADDRESSES];
static struct usb_device roots[USBFS_BUSES];
static spinlock_t devices_lock = SPINLOCK_INITIALIZER;
struct usb_completion {
	list_entry node;
	uintptr_t address;
};
struct usb_opened {
	struct usb_device *device;
	mutex_t lock;
	list_entry completed;
};

static char *descriptor_snapshot(void *data, unsigned tag, unsigned *length)
{
	struct usb_blob *blob = data;
	char *copy = malloc(blob->length);
	if (!copy)
		return NULL;
	memcpy(copy, blob->bytes, blob->length);
	*length = blob->length;
	return copy;
}
static const vfs_entry_attribute_ops descriptor_ops = {
	.snapshot = descriptor_snapshot
};
static ssize_t usbfs_read(file *fp, void *buffer, size_t size, loff_t *pos)
{
	struct usb_opened *opened = fp->f_inode->i_private;
	struct usb_device *u = opened->device;
	if (!u->online)
		return -ENODEV;
	unsigned length = sizeof(u->descriptor) + u->configuration_length;
	if (*pos < 0)
		return -EINVAL;
	if ((uint64_t)*pos >= length)
		return 0;
	if (size > length - *pos)
		size = length - *pos;
	size_t copied = 0;
	if (*pos < sizeof(u->descriptor)) {
		copied = sizeof(u->descriptor) - *pos;
		if (copied > size)
			copied = size;
		memcpy(buffer, (char *)&u->descriptor + *pos, copied);
	}
	if (size > copied)
		memcpy((char *)buffer + copied,
		       u->configuration_data + *pos + copied -
			       sizeof(u->descriptor),
		       size - copied);
	*pos += size;
	return size;
}
static int usbfs_stat(file *fp, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0664;
	st->st_rdev = fp->f_inode->i_ino;
	st->st_ino = fp->f_inode->i_ino;
	st->st_nlink = 1;
	st->st_blksize = PAGE_SIZE;
	return 0;
}
static int user_range(const void *pointer, unsigned size)
{
	return pointer && arch_mm_user_range_valid((uintptr_t)pointer, size);
}
static uintptr_t wire_pointer(const void *data, unsigned offset, int wide)
{
	uint64_t value = 0;
	memcpy(&value, (const char *)data + offset, wide ? 8 : 4);
	return (uintptr_t)value;
}
static int usbfs_ioctl(file *fp, unsigned cmd, void *arg)
{
	struct usb_opened *opened = fp->f_inode->i_private;
	struct usb_device *u = opened->device;
	unsigned nr = cmd & 0xff, size = (cmd >> 16) & 0x3fff;
	if (((cmd >> 8) & 0xff) != 'U')
		return -ENOTTY;
	if (!u->online)
		return -ENODEV;
	if ((nr == 26 && size != 4) || (nr == 8 && size != 260) ||
	    (nr == 0 && size != 16 && size != 24) ||
	    (nr == 10 && size != 44 && size != 56) ||
	    ((nr == 12 || nr == 13) && size != 4 && size != 8))
		return -EINVAL;
	if (size && !user_range(arg, size))
		return -EFAULT;
	if (nr == 26) {
		*(unsigned *)arg = 0;
		return 0;
	}
	if (nr == 31)
		return u->speed;
	if (nr == 8) {
		unsigned interface = *(unsigned *)arg;
		for (unsigned i = 0; i < u->interface_count; i++) {
			struct usb_interface *it = &u->interfaces[i];
			if (it->number == interface && it->device.driver) {
				strncpy((char *)arg + 4,
					it->device.driver->name, 255);
				((char *)arg)[259] = 0;
				return 0;
			}
		}
		return -ENODATA;
	}
	if (nr == 0) {
		int wide = size == 24;
		unsigned char *setup = arg;
		unsigned length = setup[6] | (unsigned)setup[7] << 8;
		void *data = (void *)wire_pointer(arg, wide ? 16 : 12, wide);
		if (length > USB_TRANSFER_MAX)
			return -EINVAL;
		if (length && !user_range(data, length))
			return -EFAULT;
		if (!(setup[0] & USB_DIR_IN))
			return -EPERM;
		char *copy AUTO_FREE = length ? malloc(length) : NULL;
		if (length && !copy)
			return -ENOMEM;
		int ret = usb_control(u, setup[0], setup[1],
				      setup[2] | (unsigned)setup[3] << 8,
				      setup[4] | (unsigned)setup[5] << 8, copy,
				      length);
		if (ret > 0)
			memcpy(data, copy, ret);
		return ret;
	}
	if (nr == 10) {
		int wide = size == 56;
		unsigned header = wide ? 56 : 44;
		if (!user_range(arg, header))
			return -EFAULT;
		unsigned char *urb = arg;
		if (urb[0] != 2 || (urb[1] & 0x0f))
			return -EOPNOTSUPP;
		uintptr_t pointer = wire_pointer(urb, wide ? 16 : 12, wide);
		int length;
		memcpy(&length, urb + (wide ? 24 : 16), 4);
		if (length < 8 || length > USB_TRANSFER_MAX + 8)
			return -EINVAL;
		if (!user_range((void *)pointer, length))
			return -EFAULT;
		unsigned char *copy AUTO_FREE = malloc(length);
		struct usb_completion *completion = malloc(sizeof(*completion));
		if (!copy || !completion) {
			free(completion);
			return -ENOMEM;
		}
		memcpy(copy, (void *)pointer, length);
		unsigned requested = copy[6] | (unsigned)copy[7] << 8;
		if (requested > length - 8 || !(copy[0] & USB_DIR_IN)) {
			free(completion);
			return -EPERM;
		}
		int ret = usb_control(u, copy[0], copy[1],
				      copy[2] | (unsigned)copy[3] << 8,
				      copy[4] | (unsigned)copy[5] << 8,
				      copy + 8, requested);
		int status = ret < 0 ? ret : 0, actual = ret > 0 ? ret : 0;
		if (actual)
			memcpy((void *)(pointer + 8), copy + 8, actual);
		memcpy(urb + 4, &status, 4);
		memcpy(urb + (wide ? 28 : 20), &actual, 4);
		completion->address = (uintptr_t)arg;
		mutex_lock(&opened->lock);
		list_insert_tail(&opened->completed, &completion->node);
		mutex_unlock(&opened->lock);
		return 0;
	}
	if (nr == 12 || nr == 13) {
		int wide = size == 8;
		mutex_lock(&opened->lock);
		if (list_is_empty(&opened->completed)) {
			mutex_unlock(&opened->lock);
			return -EAGAIN;
		}
		struct usb_completion *done =
			container_of(list_remove_head(&opened->completed),
				     struct usb_completion, node);
		uint64_t address = done->address;
		memcpy(arg, &address, wide ? 8 : 4);
		free(done);
		mutex_unlock(&opened->lock);
		return 0;
	}
	if (nr == 11)
		return -EINVAL;
	return -EOPNOTSUPP;
}
static unsigned usbfs_poll(file *fp, unsigned events, poll_table *table)
{
	struct usb_opened *opened = fp->f_inode->i_private;
	if (!opened->device->online)
		return FS_POLL_ERR | FS_POLL_HUP;
	mutex_lock(&opened->lock);
	unsigned ready =
		list_is_empty(&opened->completed) ? 0 : events & FS_POLL_WRITE;
	mutex_unlock(&opened->lock);
	return ready;
}
static loff_t usbfs_seek(file *fp, loff_t offset, int whence)
{
	if (whence != 0 || offset < 0)
		return -EINVAL;
	return fp->f_pos = offset;
}
static int usbfs_release(file *fp)
{
	struct usb_opened *opened = fp->f_inode->i_private;
	while (!list_is_empty(&opened->completed))
		free(container_of(list_remove_head(&opened->completed),
				  struct usb_completion, node));
	ref_count_put(opened->device);
	free(opened);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static const file_operations usbfs_fops = {
	.read = usbfs_read,
	.getattr = usbfs_stat,
	.ioctl = usbfs_ioctl,
	.poll = usbfs_poll,
	.llseek = usbfs_seek,
	.release = usbfs_release,
};
static file *usbfs_open(super_block *sb, unsigned devno, int flags)
{
	unsigned bus = MINOR(devno) / USBFS_ADDRESSES,
		 address = MINOR(devno) % USBFS_ADDRESSES;
	if (bus >= USBFS_BUSES)
		return NULL;
	int irq;
	spinlock_lock(&devices_lock, &irq);
	struct usb_device *u = devices[bus][address];
	if (u && u->online)
		ref_count_get(u);
	else
		u = NULL;
	spinlock_unlock(&devices_lock, irq);
	if (!u)
		return NULL;
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	struct usb_opened *opened = zalloc(sizeof(*opened));
	if (!fp || !node || !opened) {
		free(fp);
		free(node);
		free(opened);
		ref_count_put(u);
		return NULL;
	}
	opened->device = u;
	mutex_init(&opened->lock);
	list_init(&opened->completed);
	node->i_mode = S_IFCHR | 0664;
	node->i_ino = devno;
	node->i_private = opened;
	fp->f_inode = node;
	fp->f_fop = &usbfs_fops;
	fp->f_count = 1;
	return fp;
}
static void publish_string(struct usb_device *u, unsigned index,
			   const char *name)
{
	if (!index)
		return;
	unsigned char *raw AUTO_FREE = malloc(256);
	char *text AUTO_FREE = malloc(384);
	if (!raw || !text)
		return;
	int n = usb_control(u, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
			    0x300 | index, 0x409, raw, 255);
	if (n < 2 || raw[1] != 3 || raw[0] > n)
		return;
	unsigned length = 0;
	for (unsigned i = 2; i + 1 < raw[0]; i += 2) {
		unsigned c = raw[i] | (unsigned)raw[i + 1] << 8;
		if (c < 0x80)
			text[length++] = c ? c : '?';
		else if (c < 0x800) {
			text[length++] = 0xc0 | (c >> 6);
			text[length++] = 0x80 | (c & 63);
		} else {
			text[length++] = 0xe0 | (c >> 12);
			text[length++] = 0x80 | ((c >> 6) & 63);
			text[length++] = 0x80 | (c & 63);
		}
	}
	text[length++] = '\n';
	text[length] = 0;
	vfs_entry_text(u->sysfs_entry, name, text);
}
void usb_inventory_publish(struct usb_device *u)
{
	char text[64], path[32];
	char *event AUTO_FREE = malloc(192);
	if (!event)
		return;
	vfs_entry_node *node = u->sysfs_entry;
	struct usb_device_descriptor *d = &u->descriptor;
	struct usb_configuration_descriptor *config = (void *)u->configuration_data;
	sprintf(text, "%u\n", d->ep0_packet);
	vfs_entry_text(node, "bMaxPacketSize0", text);
	sprintf(text, "%02x\n", config->attributes);
	vfs_entry_text(node, "bmAttributes", text);
	sprintf(text, "%u\n", u->hub_ports);
	vfs_entry_text(node, "maxchild", text);
	vfs_entry_text(node, "rx_lanes", "1\n");
	vfs_entry_text(node, "tx_lanes", "1\n");
	if (config->string)
		publish_string(u, config->string, "configuration");
	else
		vfs_entry_text(node, "configuration", "\n");
	publish_string(u, d->manufacturer, "manufacturer");
	publish_string(u, d->product_string, "product");
	publish_string(u, d->serial_string, "serial");
	vfs_entry_tree *tree = device_sysfs_entries();
	struct usb_blob *blob = vfs_entry_tree_alloc(
		tree, sizeof(*blob) + sizeof(*d) + u->configuration_length);
	if (blob) {
		blob->length = sizeof(*d) + u->configuration_length;
		memcpy(blob->bytes, d, sizeof(*d));
		memcpy(blob->bytes + sizeof(*d), u->configuration_data,
		       u->configuration_length);
		vfs_entry_attribute(node, "descriptors", 0444, &descriptor_ops,
				    blob, 0);
	}
	sprintf(text, "%u\n", u->configuration);
	vfs_entry_text(node, "bConfigurationValue", text);
	sprintf(text, "%04x\n", u->usb_version);
	vfs_entry_text(node, "bcdUSB", text);
	sprintf(text, "%04x\n", d->device_version);
	vfs_entry_text(node, "bcdDevice", text);
	sprintf(text, "%02x\n", u->device_class);
	vfs_entry_text(node, "bDeviceClass", text);
	sprintf(text, "%02x\n", d->device_subclass);
	vfs_entry_text(node, "bDeviceSubClass", text);
	sprintf(text, "%02x\n", d->device_protocol);
	vfs_entry_text(node, "bDeviceProtocol", text);
	sprintf(text, "%u\n", u->interface_count);
	vfs_entry_text(node, "bNumInterfaces", text);
	sprintf(text, "%u\n", d->configurations);
	vfs_entry_text(node, "bNumConfigurations", text);
	vfs_entry_text(node, "speed",
		       u->speed == USB_SPEED_SUPER ? "5000\n" :
		       u->speed == USB_SPEED_HIGH  ? "480\n" :
		       u->speed == USB_SPEED_LOW   ? "1.5\n" :
						     "12\n");
	if (!u->busnum || u->busnum > USBFS_BUSES ||
	    u->address >= USBFS_ADDRESSES)
		return;
	unsigned minor = (u->busnum - 1) * USBFS_ADDRESSES + u->address;
	sprintf(text, "%u:%u\n", USBFS_MAJOR, minor);
	vfs_entry_text(node, "dev", text);
	sprintf(path, "bus/usb/%03u/%03u", u->busnum, u->address + 1);
	sprintf(event,
		"DEVTYPE=usb_device\nPRODUCT=%x/%x/%x\nMAJOR=%u\nMINOR=%u\nDEVNAME=%s\n",
		u->vendor, u->product, d->device_version, USBFS_MAJOR, minor,
		path);
	vfs_entry_text(node, "uevent", event);
	vfs_entry_device(devfs_entries(), path, S_IFCHR | 0664,
			 MKDEV(USBFS_MAJOR, minor), "usb", usbfs_open);
	int irq;
	spinlock_lock(&devices_lock, &irq);
	devices[u->busnum - 1][u->address] = u;
	spinlock_unlock(&devices_lock, irq);
}
void usb_inventory_remove(struct usb_device *u)
{
	if (!u->busnum || u->busnum > USBFS_BUSES ||
	    u->address >= USBFS_ADDRESSES)
		return;
	int irq;
	spinlock_lock(&devices_lock, &irq);
	devices[u->busnum - 1][u->address] = NULL;
	spinlock_unlock(&devices_lock, irq);
	char path[32];
	sprintf(path, "bus/usb/%03u/%03u", u->busnum, u->address + 1);
	vfs_entry_remove(devfs_entries(), path);
}

static const unsigned char root_configuration[] = {
	9, 2, 31, 0, 1,	   1, 0, 0xc0, 0,  9, 4,  0, 0, 1, 9, 0,
	3, 0, 7,  5, 0x81, 3, 2, 0,    12, 6, 48, 0, 0, 2, 0
};
static const unsigned char root_bos[] = { 5,	15, 22, 0,  2,	7,  16, 2,
					  2,	0,  0,	0,  10, 16, 3,	0,
					  0x0e, 0,  1,	10, 0,	0 };
static int root_control(struct usb_device *u, unsigned type, unsigned request,
			unsigned value, unsigned index, void *data,
			unsigned length)
{
	unsigned char local[32] = { 0 };
	const void *source = local;
	unsigned size = 0;
	if (!(type & USB_DIR_IN))
		return -EPERM;
	if (request == USB_REQ_GET_DESCRIPTOR) {
		unsigned descriptor = value >> 8;
		if (descriptor == 1) {
			source = &u->descriptor;
			size = sizeof(u->descriptor);
		} else if (descriptor == 2) {
			source = root_configuration;
			size = sizeof(root_configuration);
		} else if (descriptor == 15) {
			source = root_bos;
			size = sizeof(root_bos);
		} else if (descriptor == 3) {
			unsigned string = value & 255;
			if (!string) {
				local[0] = 4;
				local[1] = 3;
				local[2] = 9;
				local[3] = 4;
				size = 4;
			} else {
				const char *text =
					string == 1 ? "MOS" :
					string == 2 ? "xHCI root hub" :
						      "root";
				size = 2;
				local[1] = 3;
				while (*text && size + 1 < sizeof(local)) {
					local[size++] = *text++;
					local[size++] = 0;
				}
				local[0] = size;
			}
		} else if (descriptor == 0x2a) {
			size = 12;
			local[0] = size;
			local[1] = descriptor;
			local[2] = u->hub_ports;
			local[3] = 9;
		} else
			return -EPIPE;
	} else if (request == USB_REQ_GET_STATUS) {
		size = (type & 31) == 3 ? 4 : 2;
		if ((type & 31) == 3 && index && index <= u->hub_ports) {
			local[1] = 2;
			int irq;
			spinlock_lock(&devices_lock, &irq);
			for (unsigned i = 1; i < USBFS_ADDRESSES; i++) {
				struct usb_device *child =
					devices[u->busnum - 1][i];
				if (child && child->online && !child->parent &&
				    child->port == index) {
					local[0] = 3;
					break;
				}
			}
			spinlock_unlock(&devices_lock, irq);
		} else
			local[0] = 1;
	} else if (request == 8) {
		size = 1;
		local[0] = 1;
	} else
		return -EPIPE;
	if (length < size)
		size = length;
	if (size)
		memcpy(data, source, size);
	return size;
}
static const struct usb_host_ops root_ops = { .control = root_control };
vfs_entry_node *usb_inventory_parent(struct usb_device *u)
{
	if (u->parent)
		return u->parent->sysfs_entry;
	if (u->busnum && u->busnum <= USBFS_BUSES &&
	    roots[u->busnum - 1].sysfs_entry)
		return roots[u->busnum - 1].sysfs_entry;
	return vfs_entry_directory(vfs_entry_root(device_sysfs_entries()),
				   "devices");
}
void usb_inventory_host(unsigned busnum, unsigned pci, unsigned ports)
{
	if (!busnum || busnum > USBFS_BUSES)
		return;
	struct usb_device *u = &roots[busnum - 1];
	if (u->online)
		return;
	ref_count_init(&u->references, NULL);
	u->ops = &root_ops;
	u->busnum = busnum;
	u->address = 0;
	u->online = 1;
	u->speed = USB_SPEED_SUPER;
	u->usb_version = 0x300;
	u->device_class = 9;
	u->configuration = 1;
	u->hub_ports = ports;
	u->interface_count = 1;
	u->vendor = 0;
	u->product = 0;
	u->descriptor = (struct usb_device_descriptor){ .header = { 18, 1 },
							.usb_version = 0x300,
							.device_class = 9,
							.device_protocol = 3,
							.ep0_packet = 9,
							.vendor = u->vendor,
							.product = u->product,
							.device_version = 0x100,
							.manufacturer = 1,
							.product_string = 2,
							.serial_string = 3,
							.configurations = 1 };
	u->configuration_data = (void *)root_configuration;
	u->configuration_length = sizeof(root_configuration);
	char name[32], text[64];
	sprintf(name, "usb%u", busnum);
	u->sysfs_entry = vfs_entry_directory(
		driver_device_entry(DEVICE_BUS_PCI, pci), name);
	sprintf(u->sysfs_path, "/devices/pci0000:00/0000:%02x:%02x.%u/%s",
		pci_extract_bus(pci), pci_extract_slot(pci),
		pci_extract_func(pci), name);
	vfs_entry_node *bus = driver_bus_entry(DEVICE_BUS_USB);
	vfs_entry_link_ref(vfs_entry_directory(bus, "devices"), name,
			   u->sysfs_entry);
	vfs_entry_link(u->sysfs_entry, "subsystem", bus);
	sprintf(text, "%04x\n", u->vendor);
	vfs_entry_text(u->sysfs_entry, "idVendor", text);
	sprintf(text, "%04x\n", u->product);
	vfs_entry_text(u->sysfs_entry, "idProduct", text);
	sprintf(text, "%u\n", busnum);
	vfs_entry_text(u->sysfs_entry, "busnum", text);
	vfs_entry_text(u->sysfs_entry, "devnum", "1\n");
	vfs_entry_text(u->sysfs_entry, "devpath", "0\n");
	usb_inventory_publish(u);
}

const char *usb_inventory_parent_path(struct usb_device *u)
{
	if (u->parent)
		return u->parent->sysfs_path;
	if (u->busnum && u->busnum <= USBFS_BUSES &&
	    roots[u->busnum - 1].sysfs_entry)
		return roots[u->busnum - 1].sysfs_path;
	return "/devices";
}
