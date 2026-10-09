#ifndef MOS_FS_ENTRIES_H
#define MOS_FS_ENTRIES_H
#include <fs/vfs.h>

typedef struct vfs_entry_tree vfs_entry_tree;
typedef struct vfs_entry_node vfs_entry_node;

typedef struct {
	/* Owned per-open snapshot; freed by the generic entry file. */
	char *(*snapshot)(void *data, unsigned tag, unsigned *length);
	int (*show)(void *data, unsigned tag, char *buffer, unsigned capacity);
	ssize_t (*read)(void *data, unsigned tag, void *buffer, size_t size,
			loff_t *position);
	ssize_t (*write)(void *data, unsigned tag, const void *buffer,
			 size_t size, loff_t *position);
} vfs_entry_attribute_ops;

/* Device registration populates entries before userspace starts.
 * Child entries are VFS mounts.
 * Mounted superblocks and open files retain the tree. Release the root
 * with sb_put() on population failure or transfer it to vfs_mount(). */
vfs_entry_tree *vfs_entry_tree_create(void);
super_block *vfs_entry_tree_super(vfs_entry_tree *tree);
int vfs_entry_tree_error(vfs_entry_tree *tree);
/* Zeroed provider data, released with the last tree reference. */
void *vfs_entry_tree_alloc(vfs_entry_tree *tree, unsigned size);
const char *vfs_entry_name(vfs_entry_node *node);
vfs_entry_node *vfs_entry_child(vfs_entry_node *parent, const char *name);
vfs_entry_node *vfs_entry_root(vfs_entry_tree *tree);
vfs_entry_node *vfs_entry_directory(vfs_entry_node *parent, const char *name);
vfs_entry_node *vfs_entry_text(vfs_entry_node *parent, const char *name,
			       const char *text);
vfs_entry_node *vfs_entry_link(vfs_entry_node *parent, const char *name,
			       vfs_entry_node *target);
vfs_entry_node *vfs_entry_attribute(vfs_entry_node *parent, const char *name,
				    unsigned mode,
				    const vfs_entry_attribute_ops *ops,
				    void *data, unsigned tag);
void vfs_entry_resource(vfs_entry_node *node, uint32_t base, uint64_t size);
/* Detached entries are published explicitly with vfs_mount(). */
vfs_entry_node *vfs_entry_directory_create(vfs_entry_node *parent,
					   const char *name);
super_block *vfs_entry_super(vfs_entry_node *node);
/* Register all top-level entries before creating mount views. */
super_block *vfs_entry_tree_mount(vfs_entry_tree *tree);
vfs_entry_node *vfs_entry_directory_mode(vfs_entry_node *parent,
					 const char *name, unsigned mode);
vfs_entry_node *vfs_entry_directory_path(vfs_entry_node *parent,
					 const char *path, unsigned mode);
/* Provider ownership transfers to the entry on success. */
vfs_entry_node *vfs_entry_mount(vfs_entry_node *parent, const char *name,
				unsigned mode, super_block *provider);
void vfs_entry_set_provider(vfs_entry_node *node, const super_operations *ops);
void vfs_entry_set_mode(vfs_entry_node *node, unsigned mode);
void vfs_entry_tree_type(vfs_entry_tree *tree, unsigned magic);
vfs_entry_node *vfs_entry_device(vfs_entry_node *parent, const char *path,
				 unsigned mode, unsigned devno,
				 const char *class_name,
				 file *(*open)(super_block *, unsigned, int));
/* Filesystem roots are available to providers before they are mounted. */
vfs_entry_node *devfs_entries(void);
void vfs_entry_remove(vfs_entry_node *parent, const char *path);
#endif
