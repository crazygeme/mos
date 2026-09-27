#include <errno.h>
#include <ext4_oflags.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
#include <fs/vfs.h>
#include <hw/pci.h>
#include <lib/klib.h>
#include <macro.h>
#include <ps/ps.h>

#define SYSFS_PCI_MAX 128
#define SYSFS_BUFFER_SIZE 8192

typedef struct {
	unsigned address;
	char name[16];
	pci_resource resources[7];
} sysfs_device;

typedef struct {
	sysfs_device devices[SYSFS_PCI_MAX];
	unsigned count;
	unsigned boot_vga;
} sysfs_state;

typedef struct {
	sysfs_device *device;
	unsigned kind;
	unsigned length;
	char data[SYSFS_BUFFER_SIZE];
} sysfs_file;

enum { SYSFS_TEXT, SYSFS_CONFIG, SYSFS_ENABLE, SYSFS_RESOURCE };

static void sysfs_collect(unsigned address, uint16_t vendor, uint16_t id,
			  void *arg)
{
	sysfs_state *state = arg;
	sysfs_device *dev;
	(void)vendor;
	(void)id;
	if (state->count >= SYSFS_PCI_MAX)
		return;
	dev = &state->devices[state->count++];
	dev->address = address;
	sprintf(dev->name, "0000:%02x:%02x.%u", pci_extract_bus(address),
		pci_extract_slot(address), pci_extract_func(address));
	pci_get_resources(address, dev->resources);
	if (state->boot_vga == ~0U &&
	    pci_read_field(address, PCI_CLASS, 1) == 3 &&
	    pci_read_field(address, PCI_SUBCLASS, 1) == 0)
		state->boot_vga = address;
}

static int sysfs_getattr(file *fp, struct stat *st)
{
	sysfs_file *data = fp->f_inode->i_private;
	memset(st, 0, sizeof(*st));
	st->st_mode = fp->f_inode->i_mode;
	st->st_ino = fp->f_inode->i_ino;
	st->st_size = fp->f_inode->i_phys_size ? fp->f_inode->i_phys_size :
						 data->length;
	st->st_nlink = S_ISDIR(st->st_mode) ? 2 : 1;
	st->st_blksize = PAGE_SIZE;
	return 0;
}

static ssize_t sysfs_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	sysfs_file *data = fp->f_inode->i_private;
	unsigned i;
	if (*pos < 0)
		return -EINVAL;
	if (data->kind == SYSFS_RESOURCE)
		return -EIO;
	if ((uint64_t)*pos >= data->length)
		return 0;
	if (size > data->length - (unsigned)*pos)
		size = data->length - (unsigned)*pos;
	if (data->kind == SYSFS_CONFIG) {
		for (i = 0; i < size; i++)
			((unsigned char *)buf)[i] = pci_read_field(
				data->device->address, *pos + i, 1);
	} else {
		memcpy(buf, data->data + (unsigned)*pos, size);
	}
	*pos += size;
	return size;
}

static ssize_t sysfs_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	sysfs_file *data = fp->f_inode->i_private;
	unsigned i, command;
	if (!current->user || current->user->euid != 0)
		return -EACCES;
	if (*pos < 0)
		return -EINVAL;
	if (data->kind == SYSFS_CONFIG) {
		if ((uint64_t)*pos >= 256)
			return -EFBIG;
		if (size > 256 - (unsigned)*pos)
			size = 256 - (unsigned)*pos;
		for (i = 0; i < size; i++)
			pci_write_field(data->device->address, *pos + i, 1,
					((const unsigned char *)buf)[i]);
	} else if (data->kind == SYSFS_ENABLE) {
		if (!size)
			return 0;
		if (*(const char *)buf != '0' && *(const char *)buf != '1')
			return -EINVAL;
		command = pci_read_field(data->device->address, PCI_COMMAND, 2);
		command = *(const char *)buf == '1' ? command | 3 :
						      command & ~3U;
		pci_write_field(data->device->address, PCI_COMMAND, 2, command);
	} else {
		return -EACCES;
	}
	*pos += size;
	return size;
}

static loff_t sysfs_seek(file *fp, loff_t offset, int whence)
{
	sysfs_file *data = fp->f_inode->i_private;
	loff_t base;
	if (whence == SEEK_SET)
		base = 0;
	else if (whence == SEEK_CUR)
		base = fp->f_pos;
	else if (whence == SEEK_END)
		base = fp->f_inode->i_phys_size ? fp->f_inode->i_phys_size :
						  data->length;
	else
		return -EINVAL;
	if (offset < -base)
		return -EINVAL;
	fp->f_pos = base + offset;
	return fp->f_pos;
}

static int sysfs_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations sysfs_fops = {
	.getattr = sysfs_getattr,
	.read = sysfs_read,
	.write = sysfs_write,
	.llseek = sysfs_seek,
	.release = sysfs_release,
};

static void sysfs_entry(sysfs_file *data, const char *name)
{
	struct linux_dirent *entry;
	unsigned size = ROUND_UP(NAME_OFFSET() + strlen(name) + 1);
	if (data->length + size > sizeof(data->data))
		return;
	entry = (struct linux_dirent *)(data->data + data->length);
	entry->d_ino = 1;
	entry->d_reclen = size;
	entry->d_off = data->length + size;
	strcpy(entry->d_name, name);
	data->length += size;
}

static file *sysfs_open(super_block *sb, const char *path, int flags)
{
	sysfs_state *state = sb->s_fs_info;
	sysfs_device *dev = NULL;
	sysfs_file *data = zalloc(sizeof(*data));
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));
	const char *attr = NULL;
	unsigned i, value = 0;
	char name[24];
	int directory = 0;

	if (!data || !node || !fp) {
		free(data);
		free(node);
		free(fp);
		return NULL;
	}
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &sysfs_fops;
	fp->f_mode = flags & O_ACCMODE;
	fp->f_flag = flags;
	node->i_private = data;
	node->i_ino = 1;
	node->i_mode = S_IFREG | 0444;

	if (!*path || !strcmp(path, "/")) {
		directory = 1;
		sysfs_entry(data, "bus");
	} else if (!strcmp(path, "/bus") || !strcmp(path, "/bus/")) {
		directory = 1;
		sysfs_entry(data, "pci");
	} else if (!strcmp(path, "/bus/pci") || !strcmp(path, "/bus/pci/")) {
		directory = 1;
		sysfs_entry(data, "devices");
	} else if (!strcmp(path, "/bus/pci/devices") ||
		   !strcmp(path, "/bus/pci/devices/")) {
		directory = 1;
		for (i = 0; i < state->count; i++)
			sysfs_entry(data, state->devices[i].name);
	} else if (!strncmp(path, "/bus/pci/devices/", 17)) {
		attr = path + 17;
		for (i = 0; i < state->count; i++) {
			unsigned len = strlen(state->devices[i].name);
			if (!strncmp(attr, state->devices[i].name, len) &&
			    (attr[len] == '/' || attr[len] == 0)) {
				dev = &state->devices[i];
				attr += len;
				if (*attr == '/')
					attr++;
				break;
			}
		}
		if (!dev)
			goto missing;
		data->device = dev;
		if (!*attr) {
			static const char *entries[] = { "config",
							 "resource",
							 "vendor",
							 "device",
							 "class",
							 "revision",
							 "subsystem_vendor",
							 "subsystem_device",
							 "boot_vga",
							 "enable" };
			directory = 1;
			for (i = 0; i < sizeof(entries) / sizeof(entries[0]);
			     i++)
				sysfs_entry(data, entries[i]);
			for (i = 0; i < 6; i++) {
				if (!dev->resources[i].size ||
				    (dev->resources[i].flags & 1))
					continue;
				sprintf(name, "resource%u", i);
				sysfs_entry(data, name);
			}
		} else if (!strcmp(attr, "config")) {
			data->kind = SYSFS_CONFIG;
			data->length = 256;
			node->i_mode = S_IFREG | 0644;
		} else if (!strcmp(attr, "resource")) {
			for (i = 0; i < 7; i++) {
				pci_resource *res = &dev->resources[i];
				uint64_t end =
					res->size ? res->start + res->size - 1 :
						    0;
				data->length +=
					sprintf(data->data + data->length,
						"%08x%08x %08x%08x %08x%08x\n",
						(unsigned)(res->start >> 32),
						(unsigned)res->start,
						(unsigned)(end >> 32),
						(unsigned)end, 0U, res->flags);
			}
		} else if (!strncmp(attr, "resource", 8) && attr[8] >= '0' &&
			   attr[8] <= '5' && !attr[9]) {
			pci_resource *res = &dev->resources[attr[8] - '0'];
			if (!res->size || (res->flags & 1) ||
			    res->start + res->size > 0x100000000ULL)
				goto missing;
			data->kind = SYSFS_RESOURCE;
			node->i_phys_base = res->start;
			node->i_phys_size = res->size;
			node->i_mode = S_IFREG | 0600;
		} else {
			if (!strcmp(attr, "vendor"))
				value = pci_read_field(dev->address,
						       PCI_VENDOR_ID, 2);
			else if (!strcmp(attr, "device"))
				value = pci_read_field(dev->address,
						       PCI_DEVICE_ID, 2);
			else if (!strcmp(attr, "class"))
				value = pci_read_field(dev->address,
						       PCI_REVISION_ID, 4) >>
					8;
			else if (!strcmp(attr, "revision"))
				value = pci_read_field(dev->address,
						       PCI_REVISION_ID, 1);
			else if (!strcmp(attr, "subsystem_vendor"))
				value = pci_read_field(dev->address, 0x2c, 2);
			else if (!strcmp(attr, "subsystem_device"))
				value = pci_read_field(dev->address, 0x2e, 2);
			else if (!strcmp(attr, "boot_vga"))
				value = state->boot_vga == dev->address;
			else if (!strcmp(attr, "enable")) {
				value = !!(pci_read_field(dev->address,
							  PCI_COMMAND, 2) &
					   3);
				data->kind = SYSFS_ENABLE;
				node->i_mode = S_IFREG | 0644;
			} else
				goto missing;
			data->length = sprintf(data->data, "%u\n", value);
			if (strcmp(attr, "boot_vga") && strcmp(attr, "enable"))
				data->length =
					sprintf(data->data, "0x%06x\n", value);
		}
	} else
		goto missing;
	if (directory) {
		node->i_mode = S_IFDIR | 0555;
		sysfs_entry(data, ".");
		sysfs_entry(data, "..");
	}
	if (!(flags & O_PATH) && (flags & O_ACCMODE) != O_RDONLY &&
	    (directory ||
	     (data->kind != SYSFS_CONFIG && data->kind != SYSFS_ENABLE &&
	      data->kind != SYSFS_RESOURCE)))
		goto missing;
	return fp;
missing:
	sysfs_release(fp);
	return NULL;
}

static file *sysfs_open_root(super_block *sb, int flags)
{
	return sysfs_open(sb, "/", flags);
}

static void sysfs_release_super(super_block *sb)
{
	free(sb->s_fs_info);
	free(sb);
}

static const super_operations sysfs_sops = {
	.open = sysfs_open,
	.open_root = sysfs_open_root,
	.release = sysfs_release_super,
};

static super_block *sysfs_get_sb(const char *dev, const char *target, int flags,
				 void *arg)
{
	super_block *sb = sget(&sysfs_sops);
	sysfs_state *state = zalloc(sizeof(*state));
	(void)dev;
	(void)target;
	(void)flags;
	(void)arg;
	if (!state) {
		free(sb);
		return NULL;
	}
	state->boot_vga = ~0U;
	sb->s_fs_info = state;
	pci_scan(sysfs_collect, PCI_SCAN_ALL, state);
	return sb;
}

static fs_type sysfs_type = { .name = "sysfs", .get_sb = sysfs_get_sb };

static void sysfs_register(void)
{
	fs_register_type(&sysfs_type);
}

KERNEL_INIT(4, sysfs_register);
