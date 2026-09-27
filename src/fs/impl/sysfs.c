#include <errno.h>
#include <ext4_oflags.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
#include <fs/sysfs.h>
#include <fs/vfs.h>
#include <hw/pci.h>
#include <lib/klib.h>
#include <macro.h>
#include <ps/ps.h>

#define SYSFS_ATTRIBUTE_SIZE PAGE_SIZE
#define SYSFS_LINK_LIMIT 40

struct sysfs_node {
	struct sysfs_tree *tree;
	struct sysfs_node *parent, *children, *sibling, *next, *target;
	char *name, *text;
	unsigned mode, number, tag;
	uint32_t physical_base;
	uint64_t physical_size;
	const sysfs_attribute_ops *ops;
	void *data;
};

struct sysfs_pci {
	struct sysfs_pci *next;
	sysfs_node *node;
	unsigned address;
	pci_resource resources[7];
};

struct sysfs_tree {
	sysfs_node *root, *nodes, *pci_devices, *pci_bus;
	struct sysfs_pci *pci;
	unsigned references, next_number, boot_vga;
	int error;
};

struct sysfs_registration {
	struct sysfs_registration *next;
	sysfs_provider populate;
	void *data;
};

struct sysfs_open_file {
	sysfs_node *node;
	char *buffer;
	unsigned length;
};

static struct sysfs_registration *sysfs_providers;

static sysfs_node *sysfs_child(sysfs_node *parent, const char *name,
			       unsigned length)
{
	sysfs_node *node;
	for (node = parent->children; node; node = node->sibling)
		if (strlen(node->name) == length &&
		    !strncmp(node->name, name, length))
			return node;
	return NULL;
}

static sysfs_node *sysfs_new_node(sysfs_tree *tree, sysfs_node *parent,
				  const char *name, unsigned mode)
{
	sysfs_node *node;
	if (!tree || tree->error)
		return NULL;
	if (parent && (!S_ISDIR(parent->mode) || !*name || strchr(name, '/') ||
		       !strcmp(name, ".") || !strcmp(name, "..") ||
		       sysfs_child(parent, name, strlen(name)))) {
		tree->error = -EINVAL;
		return NULL;
	}
	node = zalloc(sizeof(*node));
	if (!node) {
		tree->error = -ENOMEM;
		return NULL;
	}
	node->name = strdup(name);
	if (!node->name) {
		free(node);
		tree->error = -ENOMEM;
		return NULL;
	}
	node->tree = tree;
	node->parent = parent;
	node->mode = mode;
	node->number = ++tree->next_number;
	node->next = tree->nodes;
	tree->nodes = node;
	if (parent) {
		node->sibling = parent->children;
		parent->children = node;
	}
	return node;
}

sysfs_node *sysfs_root(sysfs_tree *tree)
{
	return tree->root;
}

sysfs_node *sysfs_directory(sysfs_node *parent, const char *name)
{
	sysfs_node *node;
	if (!parent)
		return NULL;
	node = sysfs_child(parent, name, strlen(name));
	if (node && S_ISDIR(node->mode))
		return node;
	return sysfs_new_node(parent->tree, parent, name, S_IFDIR | 0555);
}

sysfs_node *sysfs_text(sysfs_node *parent, const char *name, const char *text)
{
	sysfs_node *node;
	if (!parent)
		return NULL;
	node = sysfs_new_node(parent->tree, parent, name, S_IFREG | 0444);
	if (!node)
		return NULL;
	node->text = strdup(text);
	if (!node->text) {
		parent->tree->error = -ENOMEM;
		return NULL;
	}
	return node;
}

sysfs_node *sysfs_link(sysfs_node *parent, const char *name, sysfs_node *target)
{
	sysfs_node *node;
	if (!parent || !target)
		return NULL;
	if (parent->tree != target->tree) {
		parent->tree->error = -EXDEV;
		return NULL;
	}
	node = sysfs_new_node(parent->tree, parent, name, S_IFLNK | 0777);
	if (node)
		node->target = target;
	return node;
}

sysfs_node *sysfs_attribute(sysfs_node *parent, const char *name, unsigned mode,
			    const sysfs_attribute_ops *ops, void *data,
			    unsigned tag)
{
	sysfs_node *node;
	if (!parent)
		return NULL;
	node = sysfs_new_node(parent->tree, parent, name,
			      S_IFREG | (mode & 0777));
	if (node) {
		node->ops = ops;
		node->data = data;
		node->tag = tag;
	}
	return node;
}

void sysfs_resource(sysfs_node *node, uint32_t base, uint64_t size)
{
	if (node) {
		node->physical_base = base;
		node->physical_size = size;
	}
}

int sysfs_register_provider(sysfs_provider populate, void *data)
{
	struct sysfs_registration *entry = zalloc(sizeof(*entry));
	if (!entry)
		return -ENOMEM;
	entry->populate = populate;
	entry->data = data;
	entry->next = sysfs_providers;
	sysfs_providers = entry;
	return 0;
}

sysfs_node *sysfs_pci_device(sysfs_tree *tree, unsigned address)
{
	struct sysfs_pci *device;
	for (device = tree->pci; device; device = device->next)
		if (device->address == address)
			return device->node;
	return NULL;
}

/* Component traversal follows node links directly, without pathname rewriting. */
static sysfs_node *sysfs_lookup(sysfs_tree *tree, const char *path,
				int nofollow)
{
	sysfs_node *node = tree->root;
	unsigned links = 0;
	while (*path) {
		const char *component;
		unsigned length;
		while (*path == '/')
			path++;
		if (!*path)
			break;
		while (S_ISLNK(node->mode)) {
			if (++links > SYSFS_LINK_LIMIT || !node->target)
				return NULL;
			node = node->target;
		}
		if (!S_ISDIR(node->mode))
			return NULL;
		component = path;
		while (*path && *path != '/')
			path++;
		length = path - component;
		if (length == 1 && component[0] == '.')
			continue;
		if (length == 2 && component[0] == '.' && component[1] == '.') {
			if (node->parent)
				node = node->parent;
			continue;
		}
		node = sysfs_child(node, component, length);
		if (!node)
			return NULL;
	}
	if (!nofollow)
		while (S_ISLNK(node->mode)) {
			if (++links > SYSFS_LINK_LIMIT || !node->target)
				return NULL;
			node = node->target;
		}
	return node;
}

/* The target is relative to the link's parent and independent of mount location. */
static int sysfs_link_text(sysfs_node *node, char *buffer, unsigned capacity)
{
	sysfs_node *part;
	unsigned suffix = 0, prefix = 0, position;
	for (part = node->target; part->parent; part = part->parent)
		suffix += strlen(part->name) + 1;
	for (part = node->parent; part->parent; part = part->parent)
		prefix += 3;
	if (prefix + suffix + 2 > capacity)
		return -ENAMETOOLONG;
	for (position = 0; position < prefix; position += 3)
		memcpy(buffer + position, "../", 3);
	position = prefix + suffix;
	buffer[position] = 0;
	for (part = node->target; part->parent; part = part->parent) {
		unsigned length = strlen(part->name);
		buffer[--position] = '/';
		position -= length;
		memcpy(buffer + position, part->name, length);
	}
	position = prefix + suffix;
	if (position)
		buffer[--position] = 0;
	else {
		buffer[0] = '.';
		buffer[1] = 0;
		position = 1;
	}
	return position;
}

static void sysfs_tree_put(sysfs_tree *tree)
{
	if (__sync_sub_and_fetch(&tree->references, 1))
		return;
	while (tree->nodes) {
		sysfs_node *node = tree->nodes;
		tree->nodes = node->next;
		free(node->name);
		free(node->text);
		free(node);
	}
	while (tree->pci) {
		struct sysfs_pci *device = tree->pci;
		tree->pci = device->next;
		free(device);
	}
	free(tree);
}

static int sysfs_getattr(file *fp, struct stat *st)
{
	struct sysfs_open_file *opened = fp->f_inode->i_private;
	sysfs_node *node = opened->node;
	memset(st, 0, sizeof(*st));
	st->st_mode = node->mode;
	st->st_ino = node->number;
	st->st_size = node->physical_size ? node->physical_size :
					    opened->length;
	st->st_nlink = S_ISDIR(node->mode) ? 2 : 1;
	st->st_blksize = PAGE_SIZE;
	return 0;
}

static ssize_t sysfs_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	struct sysfs_open_file *opened = fp->f_inode->i_private;
	sysfs_node *node = opened->node;
	if (*pos < 0)
		return -EINVAL;
	if (node->ops && node->ops->read)
		return node->ops->read(node->data, node->tag, buf, size, pos);
	if (node->physical_size)
		return -EIO;
	if ((uint64_t)*pos >= opened->length)
		return 0;
	if (size > opened->length - (unsigned)*pos)
		size = opened->length - *pos;
	memcpy(buf, (opened->buffer ? opened->buffer : node->text) + *pos,
	       size);
	*pos += size;
	return size;
}

static ssize_t sysfs_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	struct sysfs_open_file *opened = fp->f_inode->i_private;
	sysfs_node *node = opened->node;
	if (!current->user || current->user->euid || !node->ops ||
	    !node->ops->write)
		return -EACCES;
	if (*pos < 0)
		return -EINVAL;
	return node->ops->write(node->data, node->tag, buf, size, pos);
}

static loff_t sysfs_seek(file *fp, loff_t offset, int whence)
{
	struct sysfs_open_file *opened = fp->f_inode->i_private;
	loff_t base;
	if (whence == SEEK_SET)
		base = 0;
	else if (whence == SEEK_CUR)
		base = fp->f_pos;
	else if (whence == SEEK_END)
		base = opened->node->physical_size ?
			       opened->node->physical_size :
			       opened->length;
	else
		return -EINVAL;
	if (offset < -base || offset > (loff_t)0x7fffffffffffffffLL - base)
		return -EINVAL;
	fp->f_pos = base + offset;
	return fp->f_pos;
}

static int sysfs_release(file *fp)
{
	struct sysfs_open_file *opened = fp->f_inode->i_private;
	sysfs_tree_put(opened->node->tree);
	free(opened->buffer);
	free(opened);
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

static void sysfs_dirent(struct sysfs_open_file *opened, const char *name,
			 unsigned ino)
{
	struct linux_dirent *entry = (void *)(opened->buffer + opened->length);
	unsigned size = ROUND_UP(NAME_OFFSET() + strlen(name) + 1);
	entry->d_ino = ino;
	entry->d_reclen = size;
	entry->d_off = opened->length + size;
	strcpy(entry->d_name, name);
	opened->length += size;
}

static file *sysfs_open(super_block *sb, const char *path, int flags)
{
	sysfs_tree *tree = sb->s_fs_info;
	sysfs_node *node = sysfs_lookup(tree, path, flags & O_NOFOLLOW), *child;
	struct sysfs_open_file *opened = NULL;
	file *fp = NULL;
	unsigned capacity;
	int length;
	if (!node)
		return NULL;
	if (!(flags & O_PATH) && (flags & O_ACCMODE) != O_RDONLY &&
	    (!node->ops || !node->ops->write) && !node->physical_size)
		return NULL;
	fp = zalloc(sizeof(*fp));
	opened = zalloc(sizeof(*opened));
	if (!fp || !opened)
		goto fail;
	fp->f_inode = zalloc(sizeof(*fp->f_inode));
	if (!fp->f_inode)
		goto fail;
	opened->node = node;
	if (S_ISDIR(node->mode)) {
		capacity = ROUND_UP(NAME_OFFSET() + 2) +
			   ROUND_UP(NAME_OFFSET() + 3);
		for (child = node->children; child; child = child->sibling)
			capacity += ROUND_UP(NAME_OFFSET() +
					     strlen(child->name) + 1);
		opened->buffer = zalloc(capacity);
		if (!opened->buffer)
			goto fail;
		sysfs_dirent(opened, ".", node->number);
		sysfs_dirent(opened, "..",
			     node->parent ? node->parent->number :
					    node->number);
		for (child = node->children; child; child = child->sibling)
			sysfs_dirent(opened, child->name, child->number);
	} else if (S_ISLNK(node->mode) || (node->ops && node->ops->show)) {
		capacity = S_ISLNK(node->mode) ? MAX_PATH :
						 SYSFS_ATTRIBUTE_SIZE;
		opened->buffer = zalloc(capacity);
		if (!opened->buffer)
			goto fail;
		length = S_ISLNK(node->mode) ?
				 sysfs_link_text(node, opened->buffer,
						 capacity) :
				 node->ops->show(node->data, node->tag,
						 opened->buffer, capacity);
		if (length < 0 || (unsigned)length >= capacity)
			goto fail;
		opened->length = length;
	} else if (node->text)
		opened->length = strlen(node->text);
	fp->f_inode->i_mode = node->mode;
	fp->f_inode->i_ino = node->number;
	fp->f_inode->i_private = opened;
	fp->f_inode->i_phys_base = node->physical_base;
	fp->f_inode->i_phys_size = node->physical_size;
	fp->f_fop = &sysfs_fops;
	fp->f_count = 1;
	fp->f_mode = flags & O_ACCMODE;
	fp->f_flag = flags;
	__sync_add_and_fetch(&tree->references, 1);
	return fp;
fail:
	if (opened)
		free(opened->buffer);
	free(opened);
	if (fp)
		free(fp->f_inode);
	free(fp);
	return NULL;
}

static int sysfs_readlink(super_block *sb, const char *path, char *buf,
			  size_t size, size_t *count)
{
	sysfs_node *node = sysfs_lookup(sb->s_fs_info, path, 1);
	char *text;
	int length;
	if (!node)
		return -ENOENT;
	if (!S_ISLNK(node->mode))
		return -EINVAL;
	text = name_get();
	if (!text)
		return -ENOMEM;
	length = sysfs_link_text(node, text, MAX_PATH);
	if (length >= 0) {
		*count = (unsigned)length < size ? (unsigned)length : size;
		memcpy(buf, text, *count);
	}
	name_put(text);
	return length < 0 ? length : 0;
}

static file *sysfs_open_root(super_block *sb, int flags)
{
	return sysfs_open(sb, "/", flags);
}

static void sysfs_release_super(super_block *sb)
{
	sysfs_tree_put(sb->s_fs_info);
	free(sb);
}

static const super_operations sysfs_sops = {
	.open = sysfs_open,
	.open_root = sysfs_open_root,
	.readlink = sysfs_readlink,
	.release = sysfs_release_super,
};

/* PCI attributes use register descriptors, independent of pathname lookup. */
enum {
	PCI_ATTR_VENDOR,
	PCI_ATTR_DEVICE,
	PCI_ATTR_CLASS,
	PCI_ATTR_REVISION,
	PCI_ATTR_SUBVENDOR,
	PCI_ATTR_SUBDEVICE,
	PCI_ATTR_BOOT,
	PCI_ATTR_ENABLE,
	PCI_ATTR_RESOURCES,
	PCI_ATTR_CONFIG,
	PCI_ATTR_UEVENT
};

static const struct {
	const char *name;
	unsigned offset, width;
} pci_fields[] = {
	{ "vendor", PCI_VENDOR_ID, 2 },	 { "device", PCI_DEVICE_ID, 2 },
	{ "class", PCI_REVISION_ID, 4 }, { "revision", PCI_REVISION_ID, 1 },
	{ "subsystem_vendor", 0x2c, 2 }, { "subsystem_device", 0x2e, 2 },
};

static int sysfs_pci_show(void *data, unsigned tag, char *buf,
			  unsigned capacity)
{
	struct sysfs_pci *device = data;
	unsigned value, length = 0, i;
	if (capacity < 1024)
		return -ENOSPC;
	if (tag <= PCI_ATTR_SUBDEVICE) {
		value = pci_read_field(device->address, pci_fields[tag].offset,
				       pci_fields[tag].width);
		if (tag == PCI_ATTR_CLASS)
			value >>= 8;
		return sprintf(buf, "0x%06x\n", value);
	}
	switch (tag) {
	case PCI_ATTR_BOOT:
		return sprintf(buf, "%u\n",
			       device->node->tree->boot_vga == device->address);
	case PCI_ATTR_ENABLE:
		return sprintf(
			buf, "%u\n",
			!!(pci_read_field(device->address, PCI_COMMAND, 2) &
			   3));
	case PCI_ATTR_RESOURCES:
		for (i = 0; i < 7; i++) {
			pci_resource *resource = &device->resources[i];
			uint64_t end =
				resource->size ?
					resource->start + resource->size - 1 :
					0;
			length += sprintf(buf + length,
					  "%08x%08x %08x%08x %08x%08x\n",
					  (unsigned)(resource->start >> 32),
					  (unsigned)resource->start,
					  (unsigned)(end >> 32), (unsigned)end,
					  0U, resource->flags);
		}
		return length;
	case PCI_ATTR_UEVENT:
		return sprintf(buf, "PCI_SLOT_NAME=%s\n", device->node->name);
	case PCI_ATTR_CONFIG:
		return 256;
	default:
		return -EINVAL;
	}
}

static ssize_t sysfs_pci_read(void *data, unsigned tag, void *buf, size_t size,
			      loff_t *pos)
{
	struct sysfs_pci *device = data;
	unsigned i;
	(void)tag;
	if ((uint64_t)*pos >= 256)
		return 0;
	if (size > 256 - *pos)
		size = 256 - *pos;
	for (i = 0; i < size; i++)
		((unsigned char *)buf)[i] =
			pci_read_field(device->address, *pos + i, 1);
	*pos += size;
	return size;
}

static ssize_t sysfs_pci_write(void *data, unsigned tag, const void *buf,
			       size_t size, loff_t *pos)
{
	struct sysfs_pci *device = data;
	unsigned i, command;
	if (tag == PCI_ATTR_CONFIG) {
		if ((uint64_t)*pos >= 256)
			return -EFBIG;
		if (size > 256 - *pos)
			size = 256 - *pos;
		for (i = 0; i < size; i++)
			pci_write_field(device->address, *pos + i, 1,
					((const unsigned char *)buf)[i]);
	} else if (tag == PCI_ATTR_ENABLE) {
		if (!size)
			return 0;
		if (*(const char *)buf != '0' && *(const char *)buf != '1')
			return -EINVAL;
		command = pci_read_field(device->address, PCI_COMMAND, 2);
		command = *(const char *)buf == '1' ? command | 3 :
						      command & ~3U;
		pci_write_field(device->address, PCI_COMMAND, 2, command);
	} else
		return -EACCES;
	*pos += size;
	return size;
}

static const sysfs_attribute_ops pci_text_ops = { .show = sysfs_pci_show };
static const sysfs_attribute_ops pci_enable_ops = { .show = sysfs_pci_show,
						    .write = sysfs_pci_write };
static const sysfs_attribute_ops pci_config_ops = {
	.show = sysfs_pci_show,
	.read = sysfs_pci_read,
	.write = sysfs_pci_write,
};

static void sysfs_collect(unsigned address, uint16_t vendor, uint16_t id,
			  void *arg)
{
	sysfs_tree *tree = arg;
	struct sysfs_pci *device;
	char *name;
	unsigned i;
	(void)vendor;
	(void)id;
	if (tree->error)
		return;
	device = zalloc(sizeof(*device));
	name = name_get();
	if (!device || !name) {
		free(device);
		if (name)
			name_put(name);
		tree->error = -ENOMEM;
		return;
	}
	device->address = address;
	device->next = tree->pci;
	tree->pci = device;
	pci_get_resources(address, device->resources);
	if (tree->boot_vga == ~0U &&
	    pci_read_field(address, PCI_CLASS, 1) == 3 &&
	    pci_read_field(address, PCI_SUBCLASS, 1) == 0)
		tree->boot_vga = address;
	sprintf(name, "0000:%02x:%02x.%u", pci_extract_bus(address),
		pci_extract_slot(address), pci_extract_func(address));
	device->node = sysfs_directory(tree->pci_devices, name);
	if (!device->node) {
		name_put(name);
		return;
	}
	for (i = 0; i < sizeof(pci_fields) / sizeof(pci_fields[0]); i++)
		sysfs_attribute(device->node, pci_fields[i].name, 0444,
				&pci_text_ops, device, i);
	sysfs_attribute(device->node, "boot_vga", 0444, &pci_text_ops, device,
			PCI_ATTR_BOOT);
	sysfs_attribute(device->node, "enable", 0644, &pci_enable_ops, device,
			PCI_ATTR_ENABLE);
	sysfs_attribute(device->node, "resource", 0444, &pci_text_ops, device,
			PCI_ATTR_RESOURCES);
	sysfs_attribute(device->node, "config", 0644, &pci_config_ops, device,
			PCI_ATTR_CONFIG);
	sysfs_attribute(device->node, "uevent", 0444, &pci_text_ops, device,
			PCI_ATTR_UEVENT);
	sysfs_link(device->node, "subsystem", tree->pci_bus);
	for (i = 0; i < 6; i++) {
		pci_resource *resource = &device->resources[i];
		sysfs_node *node;
		if (!resource->size || (resource->flags & 1) ||
		    resource->start + resource->size > 0x100000000ULL)
			continue;
		sprintf(name, "resource%u", i);
		node = sysfs_attribute(device->node, name, 0600, NULL, device,
				       i);
		sysfs_resource(node, resource->start, resource->size);
	}
	name_put(name);
}

static super_block *sysfs_get_sb(const char *dev, const char *target, int flags,
				 void *arg)
{
	super_block *sb = sget(&sysfs_sops);
	sysfs_tree *tree = zalloc(sizeof(*tree));
	struct sysfs_registration *provider;
	(void)dev;
	(void)target;
	(void)flags;
	(void)arg;
	if (!sb || !tree) {
		free(sb);
		free(tree);
		return NULL;
	}
	tree->references = 1;
	tree->boot_vga = ~0U;
	tree->root = sysfs_new_node(tree, NULL, "", S_IFDIR | 0555);
	tree->pci_bus =
		sysfs_directory(sysfs_directory(tree->root, "bus"), "pci");
	tree->pci_devices = sysfs_directory(tree->pci_bus, "devices");
	if (!tree->root || !tree->pci_devices)
		goto fail;
	pci_scan(sysfs_collect, PCI_SCAN_ALL, tree);
	for (provider = sysfs_providers; provider && !tree->error;
	     provider = provider->next) {
		int result = provider->populate(tree, provider->data);
		if (result)
			tree->error = result;
	}
	if (tree->error)
		goto fail;
	sb->s_fs_info = tree;
	return sb;
fail:
	sysfs_tree_put(tree);
	free(sb);
	return NULL;
}

static fs_type sysfs_type = { .name = "sysfs", .get_sb = sysfs_get_sb };

static void sysfs_register(void)
{
	fs_register_type(&sysfs_type);
}

KERNEL_INIT(4, sysfs_register);
