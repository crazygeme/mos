/* USB Mass Storage Bulk-Only Transport, SCSI transparent command set, LUN 0. */
#include <device/usb/usb.h>
#include <device/blockdev.h>
#include <device/devnums.h>
#include <fs/entries.h>
#include <fs/sysfs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <lib/klib.h>
#include <errno.h>
#include "storage.h"

#define USB_DISKS 4
#define INT64_MAX_VALUE 0x7fffffffffffffffLL
#define USB_PARTS (MBR_PRIMARY_PARTITIONS + 1)
struct disk;
struct partition {
	struct disk *disk;
	uint64_t start, sectors;
	unsigned minor;
	char name[16];
};
struct disk {
	ref_count_t references;
	struct usb_interface *interface;
	struct usb_endpoint *in, *out;
	unsigned tag, index, sector_size;
	rmutex_t io_lock;
	uint64_t sectors;
	struct partition parts[USB_PARTS];
};
static struct disk *disks[USB_DISKS];
static spinlock_t disks_lock = SPINLOCK_INITIALIZER;
static void disk_release(ref_count_t *reference)
{
	struct disk *disk = (void *)reference;
	ref_count_put(disk->interface->usb);
	free(disk);
}
static void partition_release(void *data)
{
	struct partition *partition = data;
	ref_count_put(partition->disk);
}
static file *storage_open(super_block *, unsigned, int);
static void storage_disconnect(struct usb_interface *);
static void recovery(struct disk *d)
{
	struct usb_device *u = d->interface->usb;
	usb_control(u, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
		    USB_STORAGE_RESET, 0, d->interface->number, NULL, 0);
	usb_clear_halt(u, d->in);
	usb_clear_halt(u, d->out);
}
/* The caller holds the device lock across the complete BOT transaction. */
static int scsi(struct disk *d, const void *cdb, unsigned cdb_len, void *buffer,
		unsigned length, int input)
{
	struct usb_device *u = d->interface->usb;
	struct usb_storage_cbw cbw = { .signature = USB_STORAGE_CBW_SIGNATURE,
				       .tag = ++d->tag,
				       .length = length,
				       .flags = input ? USB_DIR_IN :
							USB_DIR_OUT,
				       .cdb_length = cdb_len };
	struct usb_storage_csw csw;
	unsigned done = 0;
	memcpy(cbw.cdb, cdb, cdb_len);
	int ret = usb_transfer(u, d->out, &cbw, sizeof(cbw));
	if (ret != sizeof(cbw))
		goto transport_error;
	while (done < length) {
		unsigned chunk = length - done;
		if (chunk > USB_TRANSFER_MAX)
			chunk = USB_TRANSFER_MAX;
		ret = usb_transfer(u, input ? d->in : d->out,
				   (char *)buffer + done, chunk);
		if (ret == -EPIPE) {
			if (usb_clear_halt(u, input ? d->in : d->out) < 0)
				goto transport_error;
			break;
		}
		if (ret < 0 || (unsigned)ret > chunk)
			goto transport_error;
		done += ret;
		if ((unsigned)ret < chunk)
			break;
	}
	ret = usb_transfer(u, d->in, &csw, sizeof(csw));
	if (ret == -EPIPE && !usb_clear_halt(u, d->in))
		ret = usb_transfer(u, d->in, &csw, sizeof(csw));
	if (ret != sizeof(csw) || csw.signature != USB_STORAGE_CSW_SIGNATURE ||
	    csw.tag != cbw.tag || csw.residue > length ||
	    csw.status >= USB_STORAGE_PHASE_ERROR)
		goto transport_error;
	if (csw.status != USB_STORAGE_PASSED)
		return -EIO;
	return done == length && csw.residue == 0 ? 0 : -EIO;
transport_error:
	recovery(d);
	return ret < 0 ? ret : -EIO;
}
static int disk_rw_unlocked(struct partition *p, void *buffer, uint64_t sector,
			    unsigned count, int write)
{
	struct disk *d = p->disk;
	struct usb_device *u = d->interface->usb;
	if (!u->online)
		return -ENODEV;
	if (sector > p->sectors || count > p->sectors - sector)
		return -EIO;
	sector += p->start;
	mutex_lock(&u->lock);
	int ret = 0;
	while (count && !ret) {
		unsigned max_sectors = USB_TRANSFER_MAX / d->sector_size;
		unsigned chunk = count > max_sectors ? max_sectors : count;
		struct scsi_command10 cdb = { .opcode = write ? SCSI_WRITE10 :
								SCSI_READ10 };
		usb_put_be32(cdb.lba, sector);
		cdb.count[0] = chunk >> 8;
		cdb.count[1] = chunk;
		ret = scsi(d, &cdb, sizeof(cdb), buffer, chunk * d->sector_size,
			   !write);
		sector += chunk;
		count -= chunk;
		buffer = (char *)buffer + chunk * d->sector_size;
	}
	mutex_unlock(&u->lock);
	return ret;
}
static int disk_rw(struct partition *p, void *buffer, uint64_t sector,
		   unsigned count, int write)
{
	rmutex_lock(&p->disk->io_lock);
	int ret = disk_rw_unlocked(p, buffer, sector, count, write);
	rmutex_unlock(&p->disk->io_lock);
	return ret;
}
static int storage_read(void *p, void *b, uint64_t s, unsigned n)
{
	return disk_rw(p, b, s, n, 0);
}
static int storage_write(void *p, const void *b, uint64_t s, unsigned n)
{
	return disk_rw(p, (void *)b, s, n, 1);
}
static const blockdev_io storage_ops = { .read = storage_read,
					 .write = storage_write,
					 .release = partition_release };
/* The USB interface address identifies one attachment, independent of dev_t reuse. */
static vfs_entry_node *block_directory(const char *path)
{
	return vfs_entry_directory_path(vfs_entry_root(device_sysfs_entries()),
					path, 0755);
}
static void partition_publish(struct partition *p, unsigned nr)
{
	struct disk *d = p->disk;
	vfs_entry_node *parent =
		vfs_entry_directory(d->interface->device.sysfs_entry, "block");
	if (nr)
		parent = vfs_entry_child(parent, d->parts[0].name);
	vfs_entry_node *node = vfs_entry_directory(parent, p->name);
	char text[128];
	sprintf(text, "%u:%u\n", USB_STORAGE_MAJOR, p->minor);
	vfs_entry_text(node, "dev", text);
	sprintf(text, "%llu\n", p->sectors);
	vfs_entry_text(node, "size", text);
	vfs_entry_text(node, "removable", "1\n");
	vfs_entry_text(node, "ro", "0\n");
	sprintf(text, "%u\n", d->interface->device.address);
	vfs_entry_text(node, "mos_generation", text);
	if (nr) {
		sprintf(text, "%u\n", nr);
		vfs_entry_text(node, "partition", text);
		sprintf(text, "%llu\n", p->start);
		vfs_entry_text(node, "start", text);
	}
	sprintf(text, "MAJOR=%u\nMINOR=%u\nDEVNAME=%s\nDEVTYPE=%s\n",
		USB_STORAGE_MAJOR, p->minor, p->name,
		nr ? "partition" : "disk");
	vfs_entry_text(node, "uevent", text);
	vfs_entry_link_ref(block_directory("class/block"), p->name, node);
	sprintf(text, "%u:%u", USB_STORAGE_MAJOR, p->minor);
	vfs_entry_link_ref(block_directory("dev/block"), text, node);
	if (!nr)
		vfs_entry_link_ref(block_directory("block"), p->name, node);
}
static void partition_unpublish(struct partition *p, unsigned nr)
{
	char dev[32];
	sprintf(dev, "%u:%u", USB_STORAGE_MAJOR, p->minor);
	vfs_entry_remove(block_directory("class/block"), p->name);
	vfs_entry_remove(block_directory("dev/block"), dev);
	if (!nr)
		vfs_entry_remove(block_directory("block"), p->name);
}
static int partition_register(struct disk *d, unsigned nr, uint64_t start,
			      uint64_t count)
{
	struct partition *p = &d->parts[nr];
	p->disk = d;
	p->start = start;
	p->sectors = count;
	p->minor = d->index * USB_STORAGE_MINORS_PER_DISK + nr;
	if (nr)
		sprintf(p->name, "sd%c%u", 'a' + d->index, nr);
	else
		sprintf(p->name, "sd%c", 'a' + d->index);
	blockdev_register(p->name, USB_STORAGE_MAJOR, p->minor,
			  count * d->sector_size, BLOCKDEV_FLAG_MOUNTABLE);
	int ret = blockdev_bind_io(p->name, d->sector_size, &storage_ops, p);
	if (ret) {
		blockdev_unregister(p->name);
		return ret;
	}
	ref_count_get(d);
	vfs_entry_device(devfs_entries(), p->name, S_IFBLK | 0660,
			 MKDEV(USB_STORAGE_MAJOR, p->minor), "sd",
			 storage_open);
	partition_publish(p, nr);
	return 0;
}
static int storage_probe(struct usb_interface *interface)
{
	unsigned index;
	for (index = 0; index < USB_DISKS; index++)
		if (!disks[index])
			break;
	if (index == USB_DISKS)
		return -ENOSPC;
	struct disk *d = zalloc(sizeof(*d));
	if (!d)
		return -ENOMEM;
	ref_count_init(&d->references, disk_release);
	ref_count_get(interface->usb);
	d->interface = interface;
	d->index = index;
	rmutex_init(&d->io_lock);
	for (unsigned i = 0; i < interface->endpoint_count; i++) {
		struct usb_endpoint *e = interface->endpoints[i];
		if ((e->attributes & USB_ENDPOINT_TYPE_MASK) != USB_XFER_BULK)
			continue;
		if (e->address & USB_DIR_IN)
			d->in = e;
		else
			d->out = e;
	}
	if (!d->in || !d->out) {
		ref_count_put(d);
		return -EINVAL;
	}
	unsigned char max_lun = USB_DEFAULT_LUN;
	int ret = usb_control(interface->usb,
			      USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			      USB_STORAGE_GET_MAX_LUN, 0, interface->number,
			      &max_lun, sizeof(max_lun));
	if (ret < 0 && ret != -EPIPE) {
		ref_count_put(d);
		return ret;
	}
	mutex_lock(&interface->usb->lock);
	struct scsi_inquiry_data inquiry;
	struct scsi_command6 inquiry_cmd = {
		.opcode = SCSI_INQUIRY, .allocation_length = sizeof(inquiry)
	};
	ret = scsi(d, &inquiry_cmd, sizeof(inquiry_cmd), &inquiry,
		   sizeof(inquiry), 1);
	if (ret || (inquiry.device_type & SCSI_DEVICE_TYPE_MASK) !=
			   SCSI_DEVICE_DIRECT_ACCESS) {
		ret = -EOPNOTSUPP;
		goto failed;
	}
	/* Unit Attention is consumed before capacity is queried. */
	for (unsigned attempt = 0; attempt < USB_STORAGE_READY_ATTEMPTS;
	     attempt++) {
		struct scsi_command6 ready = { .opcode = SCSI_TEST_UNIT_READY };
		ret = scsi(d, &ready, sizeof(ready), NULL, 0, 0);
		if (!ret)
			break;
		struct scsi_sense_data sense;
		struct scsi_command6 sense_cmd = { .opcode = SCSI_REQUEST_SENSE,
						   .allocation_length =
							   sizeof(sense) };
		scsi(d, &sense_cmd, sizeof(sense_cmd), &sense, sizeof(sense),
		     1);
	}
	if (ret)
		goto failed;
	struct scsi_capacity10 capacity;
	struct scsi_command10 capacity_cmd = { .opcode = SCSI_READ_CAPACITY10 };
	ret = scsi(d, &capacity_cmd, sizeof(capacity_cmd), &capacity,
		   sizeof(capacity), 1);
	if (ret)
		goto failed;
	d->sectors = (uint64_t)usb_get_be32(capacity.last_lba) + 1;
	d->sector_size = usb_get_be32(capacity.block_size);
	if (d->sector_size != USB_STORAGE_SECTOR_SIZE ||
	    d->sectors > SCSI_READ10_MAX_LBA) {
		ret = -EOPNOTSUPP;
		goto failed;
	}

	mutex_unlock(&interface->usb->lock);
	int irq;
	spinlock_lock(&disks_lock, &irq);
	disks[d->index] = d;
	spinlock_unlock(&disks_lock, irq);
	interface->class_data = d;
	ret = partition_register(d, 0, 0, d->sectors);
	if (ret) {
		storage_disconnect(interface);
		return ret;
	}
	struct mbr_sector mbr;
	if (!disk_rw(&d->parts[0], &mbr, 0, 1, 0) &&
	    mbr.signature == MBR_SIGNATURE) {
		for (unsigned i = 0; i < MBR_PRIMARY_PARTITIONS; i++) {
			struct mbr_partition *entry = &mbr.partitions[i];
			uint64_t start = usb_get_le32(entry->start),
				 count = usb_get_le32(entry->sectors);
			if (!entry->type ||
			    entry->type == MBR_TYPE_EXTENDED_CHS ||
			    entry->type == MBR_TYPE_EXTENDED_LBA ||
			    entry->type == MBR_TYPE_PROTECTIVE_GPT || !count ||
			    !start || start >= d->sectors ||
			    count > d->sectors - start)
				continue;
			partition_register(d, i + 1, start, count);
		}
	}
	printk("usb-storage: /dev/sd%c sectors=%llu sector_size=%u LUN=0\n",
	       'a' + d->index, d->sectors, d->sector_size);
	return 0;
failed:
	mutex_unlock(&interface->usb->lock);
	ref_count_put(d);
	return ret;
}
static ssize_t file_rw(file *fp, void *buffer, size_t size, loff_t *pos,
		       int write)
{
	struct partition *p = fp->f_inode->i_private;
	uint64_t length = p->sectors * USB_STORAGE_SECTOR_SIZE;
	if (*pos < 0)
		return -EINVAL;
	if (!p->disk->interface->usb->online)
		return -ENODEV;
	if ((uint64_t)*pos >= length)
		return write && size ? -ENOSPC : 0;
	if (size > length - *pos)
		size = length - *pos;
	unsigned char *scratch = malloc(USB_STORAGE_SECTOR_SIZE);
	if (!scratch)
		return -ENOMEM;
	size_t done = 0;
	int ret = 0;
	/* Serialize partial-sector read/modify/write across all opens. */
	rmutex_lock(&p->disk->io_lock);
	while (done < size) {
		uint64_t offset = *pos + done;
		unsigned within = offset % USB_STORAGE_SECTOR_SIZE,
			 chunk = size - done;
		if (chunk > USB_STORAGE_SECTOR_SIZE - within)
			chunk = USB_STORAGE_SECTOR_SIZE - within;
		if (write && !within && chunk == USB_STORAGE_SECTOR_SIZE)
			ret = disk_rw(p, (char *)buffer + done,
				      offset / USB_STORAGE_SECTOR_SIZE, 1, 1);
		else {
			ret = disk_rw(p, scratch,
				      offset / USB_STORAGE_SECTOR_SIZE, 1, 0);
			if (!ret) {
				if (write) {
					memcpy(scratch + within,
					       (char *)buffer + done, chunk);
					ret = disk_rw(
						p, scratch,
						offset /
							USB_STORAGE_SECTOR_SIZE,
						1, 1);
				} else
					memcpy((char *)buffer + done,
					       scratch + within, chunk);
			}
		}
		if (ret)
			break;
		done += chunk;
	}
	rmutex_unlock(&p->disk->io_lock);
	free(scratch);
	*pos += done;
	return done ? (ssize_t)done : ret;
}
static ssize_t node_read(file *f, void *b, size_t n, loff_t *p)
{
	return file_rw(f, b, n, p, 0);
}
static ssize_t node_write(file *f, const void *b, size_t n, loff_t *p)
{
	return file_rw(f, (void *)b, n, p, 1);
}
static int node_ioctl(file *f, unsigned command, void *buffer)
{
	struct partition *p = f->f_inode->i_private;
	switch (command) {
	case BLKGETSIZE:
		*(unsigned long *)buffer = p->sectors;
		return 0;
	case BLKGETSIZE64:
		*(uint64_t *)buffer = p->sectors * USB_STORAGE_SECTOR_SIZE;
		return 0;
	case BLKSSZGET:
		*(int *)buffer = USB_STORAGE_SECTOR_SIZE;
		return 0;
	case BLKFLSBUF: {
		struct scsi_command10 cdb = {
			.opcode = SCSI_SYNCHRONIZE_CACHE10
		};
		mutex_lock(&p->disk->interface->usb->lock);
		int ret = scsi(p->disk, &cdb, sizeof(cdb), NULL, 0, 0);
		mutex_unlock(&p->disk->interface->usb->lock);
		return ret;
	}
	default:
		return -ENOTTY;
	}
}
static int node_getattr(file *f, struct stat *s)
{
	struct partition *p = f->f_inode->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFBLK | 0660;
	s->st_rdev = MKDEV(USB_STORAGE_MAJOR, p->minor);
	s->st_size = p->sectors * USB_STORAGE_SECTOR_SIZE;
	s->st_blksize = USB_STORAGE_SECTOR_SIZE;
	s->st_blocks = p->sectors;
	s->st_nlink = 1;
	return 0;
}
static loff_t node_seek(file *f, loff_t offset, int whence)
{
	struct partition *p = f->f_inode->i_private;
	loff_t base =
		whence == SEEK_SET ? 0 :
		whence == SEEK_CUR ? f->f_pos :
		whence == SEEK_END ? p->sectors * USB_STORAGE_SECTOR_SIZE :
				     -1;
	if (base < 0 || (offset > 0 && base > INT64_MAX_VALUE - offset) ||
	    offset < -base)
		return -EINVAL;
	f->f_pos = base + offset;
	return f->f_pos;
}
static int node_release(file *f)
{
	struct partition *partition = f->f_inode->i_private;
	ref_count_put(partition->disk);
	free(f->f_inode);
	free(f);
	return 0;
}
static const file_operations storage_fops = { .read = node_read,
					      .write = node_write,
					      .ioctl = node_ioctl,
					      .getattr = node_getattr,
					      .llseek = node_seek,
					      .release = node_release };
static file *storage_open(super_block *sb, unsigned rdev, int flags)
{
	(void)sb;
	unsigned disk = MINOR(rdev) / USB_STORAGE_MINORS_PER_DISK,
		 nr = MINOR(rdev) % USB_STORAGE_MINORS_PER_DISK;
	if (disk >= USB_DISKS || nr >= USB_PARTS)
		return NULL;
	int irq;
	spinlock_lock(&disks_lock, &irq);
	struct disk *device = disks[disk];
	if (device && device->parts[nr].sectors &&
	    device->interface->usb->online)
		ref_count_get(device);
	else
		device = NULL;
	spinlock_unlock(&disks_lock, irq);
	if (!device)
		return NULL;
	file *f = zalloc(sizeof(*f));
	inode *node = zalloc(sizeof(*node));
	if (!f || !node) {
		free(f);
		free(node);
		ref_count_put(device);
		return NULL;
	}
	node->i_mode = S_IFBLK | 0660;
	node->i_private = &device->parts[nr];
	f->f_inode = node;
	f->f_count = 1;
	f->f_fop = &storage_fops;
	f->f_mode = flags & O_ACCMODE;
	f->f_flag = flags;
	return f;
}
static void storage_disconnect(struct usb_interface *interface)
{
	struct disk *disk = interface->class_data;
	if (!disk)
		return;
	rmutex_lock(&disk->io_lock);
	int irq;
	spinlock_lock(&disks_lock, &irq);
	if (disks[disk->index] == disk)
		disks[disk->index] = NULL;
	spinlock_unlock(&disks_lock, irq);
	for (unsigned i = 0; i < USB_PARTS; i++) {
		struct partition *partition = &disk->parts[i];
		if (!partition->sectors)
			continue;
		partition_unpublish(partition, i);
		vfs_entry_remove(devfs_entries(), partition->name);
		blockdev_unregister(partition->name);
	}
	rmutex_unlock(&disk->io_lock);
	interface->class_data = NULL;
	ref_count_put(disk);
}
static void storage_flush(void)
{
	for (unsigned i = 0; i < USB_DISKS; i++) {
		int irq;
		spinlock_lock(&disks_lock, &irq);
		struct disk *disk = disks[i];
		if (disk)
			ref_count_get(disk);
		spinlock_unlock(&disks_lock, irq);
		if (!disk)
			continue;
		if (disk->interface->usb->online) {
			struct scsi_command10 command = {
				.opcode = SCSI_SYNCHRONIZE_CACHE10
			};
			mutex_lock(&disk->interface->usb->lock);
			scsi(disk, &command, sizeof(command), NULL, 0, 0);
			mutex_unlock(&disk->interface->usb->lock);
		}
		ref_count_put(disk);
	}
}
static void storage_backend_init(void)
{
	static const blockdev_backend backend = { .flush = storage_flush,
						  .close = storage_flush };
	blockdev_register_backend(&backend);
}
#include <macro.h>
KERNEL_INIT(2, storage_backend_init);
static driver_t storage_driver = { .name = "usb-storage",
				   .bus = DEVICE_BUS_USB,
				   .usb_class = USB_CLASS_STORAGE,
				   .usb_subclass = USB_STORAGE_SCSI,
				   .usb_protocol = USB_STORAGE_BULK_ONLY,
				   .probe_usb = storage_probe,
				   .disconnect_usb = storage_disconnect };
DRIVER_REGISTER(storage_driver);
