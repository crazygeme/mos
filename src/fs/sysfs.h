#ifndef MOS_FS_SYSFS_H
#define MOS_FS_SYSFS_H
#include <fs/fs.h>

typedef struct sysfs_tree sysfs_tree;
typedef struct sysfs_node sysfs_node;

typedef struct {
	int (*show)(void *data, unsigned tag, char *buffer, unsigned capacity);
	ssize_t (*read)(void *data, unsigned tag, void *buffer, size_t size,
			loff_t *position);
	ssize_t (*write)(void *data, unsigned tag, const void *buffer,
			 size_t size, loff_t *position);
} sysfs_attribute_ops;

/* A provider populates an immutable tree for each sysfs mount. */
typedef int (*sysfs_provider)(sysfs_tree *tree, void *data);
int sysfs_register_provider(sysfs_provider populate, void *data);
sysfs_node *sysfs_root(sysfs_tree *tree);
sysfs_node *sysfs_directory(sysfs_node *parent, const char *name);
sysfs_node *sysfs_text(sysfs_node *parent, const char *name, const char *text);
sysfs_node *sysfs_link(sysfs_node *parent, const char *name,
		       sysfs_node *target);
sysfs_node *sysfs_attribute(sysfs_node *parent, const char *name, unsigned mode,
			    const sysfs_attribute_ops *ops, void *data,
			    unsigned tag);
void sysfs_resource(sysfs_node *node, uint32_t base, uint64_t size);
sysfs_node *sysfs_pci_device(sysfs_tree *tree, unsigned address);
#endif
