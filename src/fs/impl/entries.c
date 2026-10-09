#include <device/chardev.h>
#include <device/blockdev.h>
#include <device/devnode.h>
#include <errno.h>
#include <fs/fcntl.h>
#include <fs/entries.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <macro.h>
#include <ps/ps.h>

#define ENTRY_ATTRIBUTE_SIZE PAGE_SIZE

struct vfs_entry_node {
	struct vfs_entry_tree *tree;
	struct vfs_entry_node *parent, *target;
	list_entry list;
	super_block *sb, *provider;
	const super_operations *provider_ops;
	char *name, *text;
	unsigned mode, number, tag;
	uint32_t physical_base;
	uint64_t physical_size;
	const vfs_entry_attribute_ops *ops;
	void *data;
};

struct entry_allocation {
	list_entry list;
};

struct vfs_entry_tree {
	vfs_entry_node *root;
	list_entry nodes, allocations;
	unsigned references, next_number;
	int error;
	unsigned magic;
	mutex_t lock;
};

struct vfs_entry_open_file {
	vfs_entry_node *node;
	super_block *parent;
	char *buffer;
	unsigned length;
};

static const super_operations entry_sops;

vfs_entry_node *vfs_entry_child(vfs_entry_node *parent, const char *name)
{
	struct rb_node *cursor;
	vfs_entry_node *result = NULL;
	if (!parent || !parent->sb || !name)
		return NULL;
	mutex_lock(&parent->sb->s_lock);
	cursor = parent->sb->s_mounts.rb_node;
	while (cursor) {
		vfs_mount_node *mount =
			rb_entry(cursor, vfs_mount_node, rb_node);
		int order = strcmp(mount->path + 1, name);
		if (!order) {
			if (mount->sb->s_op == &entry_sops)
				result = mount->sb->s_fs_info;
			break;
		}
		cursor = order < 0 ? cursor->rb_left : cursor->rb_right;
	}
	mutex_unlock(&parent->sb->s_lock);
	return result;
}

static vfs_entry_node *vfs_entry_new_node(vfs_entry_tree *tree,
					  vfs_entry_node *parent,
					  const char *name, unsigned mode,
					  int attach)
{
	vfs_entry_node *node;
	if (!tree || tree->error)
		return NULL;
	if (!name) {
		tree->error = -EINVAL;
		return NULL;
	}
	if (strlen(name) > 255 ||
	    (parent && strlen(parent->sb->s_mountpoint) + strlen(name) + 1 >=
			       sizeof(parent->sb->s_mountpoint))) {
		tree->error = -ENAMETOOLONG;
		return NULL;
	}
	if (parent && (!S_ISDIR(parent->mode) || !*name || strchr(name, '/') ||
		       !strcmp(name, ".") || !strcmp(name, "..") ||
		       vfs_entry_child(parent, name))) {
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
	node->sb = sget(&entry_sops);
	if (!node->sb) {
		if (node->provider)
			sb_put(node->provider);
		free(node->name);
		free(node);
		tree->error = -ENOMEM;
		return NULL;
	}
	node->sb->s_fs_info = node;
	node->tree = tree;
	node->parent = parent;
	node->mode = mode;
	mutex_lock(&tree->lock);
	node->number = ++tree->next_number;
	list_insert_head(&tree->nodes, &node->list);
	mutex_unlock(&tree->lock);
	if (parent && attach) {
		char *path = name_get();
		int result;
		if (!path) {
			tree->error = -ENOMEM;
			free(node->sb);
			return NULL;
		}
		sprintf(path, "/%s", name);
		result = vfs_mount(parent->sb, path, node->sb);
		name_put(path);
		if (result) {
			tree->error = result;
			free(node->sb);
			return NULL;
		}
	}
	if (parent)
		__sync_add_and_fetch(&tree->references, 1);
	return node;
}

vfs_entry_node *vfs_entry_root(vfs_entry_tree *tree)
{
	return tree->root;
}

vfs_entry_node *vfs_entry_directory(vfs_entry_node *parent, const char *name)
{
	vfs_entry_node *node;
	if (!parent)
		return NULL;
	node = vfs_entry_child(parent, name);
	if (node && S_ISDIR(node->mode))
		return node;
	return vfs_entry_new_node(parent->tree, parent, name, S_IFDIR | 0555,
				  1);
}

vfs_entry_node *vfs_entry_text(vfs_entry_node *parent, const char *name,
			       const char *text)
{
	vfs_entry_node *node;
	if (!parent)
		return NULL;
	node = vfs_entry_new_node(parent->tree, parent, name, S_IFREG | 0444,
				  1);
	if (!node)
		return NULL;
	node->text = strdup(text);
	if (!node->text) {
		parent->tree->error = -ENOMEM;
		return NULL;
	}
	return node;
}

vfs_entry_node *vfs_entry_link(vfs_entry_node *parent, const char *name,
			       vfs_entry_node *target)
{
	vfs_entry_node *node;
	if (!parent || !target)
		return NULL;
	if (parent->tree != target->tree) {
		parent->tree->error = -EXDEV;
		return NULL;
	}
	/* Targets must be real entries, so alias chains cannot form cycles. */
	if (S_ISLNK(target->mode)) {
		parent->tree->error = -EINVAL;
		return NULL;
	}
	node = vfs_entry_new_node(parent->tree, parent, name, S_IFLNK | 0777,
				  1);
	if (node)
		node->target = target;
	return node;
}

vfs_entry_node *vfs_entry_attribute(vfs_entry_node *parent, const char *name,
				    unsigned mode,
				    const vfs_entry_attribute_ops *ops,
				    void *data, unsigned tag)
{
	vfs_entry_node *node;
	if (!parent)
		return NULL;
	node = vfs_entry_new_node(parent->tree, parent, name,
				  S_IFREG | (mode & 0777), 1);
	if (node) {
		node->ops = ops;
		node->data = data;
		node->tag = tag;
	}
	return node;
}

void vfs_entry_resource(vfs_entry_node *node, uint32_t base, uint64_t size)
{
	if (node) {
		node->physical_base = base;
		node->physical_size = size;
	}
}

/* The target is relative to the link's parent and independent of mount location. */
static int vfs_entry_link_text(vfs_entry_node *node, char *buffer,
			       unsigned capacity)
{
	vfs_entry_node *part;
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

static void vfs_entry_tree_put(vfs_entry_tree *tree)
{
	if (__sync_sub_and_fetch(&tree->references, 1))
		return;
	while (!list_is_empty(&tree->nodes)) {
		vfs_entry_node *node = container_of(
			list_remove_head(&tree->nodes), vfs_entry_node, list);
		free(node->name);
		free(node->text);
		free(node);
	}
	while (!list_is_empty(&tree->allocations)) {
		struct entry_allocation *allocation =
			container_of(list_remove_head(&tree->allocations),
				     struct entry_allocation, list);
		free(allocation);
	}
	free(tree);
}

static int vfs_entry_getattr(file *fp, struct stat *st)
{
	struct vfs_entry_open_file *opened = fp->f_inode->i_private;
	vfs_entry_node *node = opened->node;
	memset(st, 0, sizeof(*st));
	st->st_mode = node->mode;
	st->st_ino = node->number;
	st->st_size = node->physical_size ? node->physical_size :
					    opened->length;
	st->st_nlink = S_ISDIR(node->mode) ? 2 : 1;
	st->st_blksize = PAGE_SIZE;
	return 0;
}

static ssize_t vfs_entry_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	struct vfs_entry_open_file *opened = fp->f_inode->i_private;
	vfs_entry_node *node = opened->node;
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

static ssize_t vfs_entry_write(file *fp, const void *buf, size_t size,
			       loff_t *pos)
{
	struct vfs_entry_open_file *opened = fp->f_inode->i_private;
	vfs_entry_node *node = opened->node;
	if (!current->fs || current->credentials->euid || !node->ops ||
	    !node->ops->write)
		return -EACCES;
	if (*pos < 0)
		return -EINVAL;
	return node->ops->write(node->data, node->tag, buf, size, pos);
}

static loff_t vfs_entry_seek(file *fp, loff_t offset, int whence)
{
	struct vfs_entry_open_file *opened = fp->f_inode->i_private;
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

static int vfs_entry_release(file *fp)
{
	struct vfs_entry_open_file *opened = fp->f_inode->i_private;
	super_block *parent = opened->parent;
	vfs_entry_tree_put(opened->node->tree);
	free(opened->buffer);
	free(opened);
	free(fp->f_inode);
	free(fp);
	if (parent)
		sb_put(parent);
	return 0;
}

static file *entry_open_root(super_block *sb, int flags);

static file *vfs_entry_follow_link(file *fp, int flags)
{
	struct vfs_entry_open_file *opened = fp->f_inode->i_private;
	vfs_entry_node *target = opened->node->target;
	return target && target->sb ? entry_open_root(target->sb, flags) : NULL;
}

static int vfs_entry_notify_parent(file *fp, super_block **owner, uint64_t *ino,
				   const char **name)
{
	struct vfs_entry_open_file *opened = fp->f_inode->i_private;
	if (!opened->parent)
		return 0;
	*owner = opened->parent;
	*ino = opened->node->parent->number;
	*name = opened->node->name;
	return 1;
}

static const file_operations vfs_entry_fops = {
	.getattr = vfs_entry_getattr,
	.notify_parent = vfs_entry_notify_parent,
	.follow_link = vfs_entry_follow_link,
	.read = vfs_entry_read,
	.write = vfs_entry_write,
	.llseek = vfs_entry_seek,
	.release = vfs_entry_release,
};

static void vfs_entry_dirent(struct vfs_entry_open_file *opened,
			     const char *name, unsigned ino)
{
	struct linux_dirent *entry = (void *)(opened->buffer + opened->length);
	unsigned size = ROUND_UP(NAME_OFFSET() + strlen(name) + 1);
	entry->d_ino = ino;
	entry->d_reclen = size;
	entry->d_off = opened->length + size;
	strcpy(entry->d_name, name);
	opened->length += size;
}

static file *entry_open_root(super_block *sb, int flags)
{
	vfs_entry_node *node = sb->s_fs_info;
	vfs_entry_tree *tree = node->tree;
	struct rb_node *cursor;
	struct vfs_entry_open_file *opened = NULL;
	file *fp = NULL;
	unsigned capacity;
	int length;
	if (node->provider && node->provider->s_op->open_root) {
		fp = node->provider->s_op->open_root(node->provider, flags);
		if (fp && fp->f_inode && !fp->f_inode->i_ino)
			fp->f_inode->i_ino = node->number;
		return fp;
	}
	if (node->provider_ops && node->provider_ops->open_root) {
		fp = node->provider_ops->open_root(sb, flags);
		if (fp && fp->f_inode && !fp->f_inode->i_ino)
			fp->f_inode->i_ino = node->number;
		return fp;
	}
	if (S_ISLNK(node->mode) && !(flags & O_NOFOLLOW))
		return vfs_open(node->target->sb, "", flags);
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
		mutex_lock(&sb->s_lock);
		capacity = ROUND_UP(NAME_OFFSET() + 2) +
			   ROUND_UP(NAME_OFFSET() + 3);
		for (cursor = rb_first(&sb->s_mounts); cursor;
		     cursor = rb_next(cursor)) {
			vfs_mount_node *mount =
				rb_entry(cursor, vfs_mount_node, rb_node);
			capacity += ROUND_UP(NAME_OFFSET() +
					     strlen(mount->path + 1) + 1);
		}
		opened->buffer = zalloc(capacity);
		if (!opened->buffer) {
			mutex_unlock(&sb->s_lock);
			goto fail;
		}
		vfs_entry_dirent(opened, ".", node->number);
		vfs_entry_dirent(opened, "..",
				 node->parent ? node->parent->number :
						node->number);
		for (cursor = rb_first(&sb->s_mounts); cursor;
		     cursor = rb_next(cursor)) {
			vfs_mount_node *mount =
				rb_entry(cursor, vfs_mount_node, rb_node);
			vfs_entry_node *child = mount->sb->s_fs_info;
			vfs_entry_dirent(opened, mount->path + 1,
					 mount->sb->s_op == &entry_sops ?
						 child->number :
						 0);
		}
		mutex_unlock(&sb->s_lock);
	} else if (node->ops && node->ops->snapshot) {
		opened->buffer = node->ops->snapshot(node->data, node->tag,
						     &opened->length);
		if (!opened->buffer)
			goto fail;
	} else if (S_ISLNK(node->mode) || (node->ops && node->ops->show)) {
		capacity = S_ISLNK(node->mode) ? MAX_PATH :
						 ENTRY_ATTRIBUTE_SIZE;
		opened->buffer = zalloc(capacity);
		if (!opened->buffer)
			goto fail;
		length = S_ISLNK(node->mode) ?
				 vfs_entry_link_text(node, opened->buffer,
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
	fp->f_inode->i_size = node->physical_size ? node->physical_size :
						    opened->length;
	fp->f_inode->i_private = opened;
	fp->f_inode->i_phys_base = node->physical_base;
	fp->f_inode->i_phys_size = node->physical_size;
	fp->f_fop = &vfs_entry_fops;
	fp->f_count = 1;
	fp->f_mode = flags & O_ACCMODE;
	fp->f_flag = flags;
	__sync_add_and_fetch(&tree->references, 1);
	if (node->parent && node->parent->sb) {
		opened->parent = node->parent->sb;
		sb_get(opened->parent);
	}
	vfs_set_file_origin(fp, node->sb, "");
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

static int vfs_entry_readlink(super_block *sb, const char *path, char *buf,
			      size_t size, size_t *count)
{
	vfs_entry_node *node = sb->s_fs_info;
	char *text;
	int length;
	if (node->provider && node->provider->s_op->readlink)
		return node->provider->s_op->readlink(node->provider, path, buf,
						      size, count);
	if (node->provider_ops && node->provider_ops->readlink)
		return node->provider_ops->readlink(sb, path, buf, size, count);
	if (*path && strcmp(path, "/")) {
		if (S_ISLNK(node->mode))
			return vfs_readlink(node->target->sb, path, buf, size,
					    count);
		return -ENOENT;
	}
	if (!S_ISLNK(node->mode))
		return -EINVAL;
	text = name_get();
	if (!text)
		return -ENOMEM;
	length = vfs_entry_link_text(node, text, MAX_PATH);
	if (length >= 0) {
		unsigned copied = (unsigned)length < size ? (unsigned)length :
							    size;
		if (count)
			*count = copied;
		memcpy(buf, text, copied);
	}
	name_put(text);
	return length < 0 ? length : 0;
}

/* Mount resolution is performed by VFS; only aliases delegate a suffix. */
static file *entry_open(super_block *sb, const char *path, int flags)
{
	vfs_entry_node *node = sb->s_fs_info;
	if (node->provider && node->provider->s_op->open)
		return node->provider->s_op->open(node->provider, path, flags);
	if (node->provider_ops && node->provider_ops->open)
		return node->provider_ops->open(sb, path, flags);
	if (S_ISLNK(node->mode))
		return vfs_open(node->target->sb, path, flags);
	return NULL;
}

static void entry_release_super(super_block *sb)
{
	vfs_entry_node *node = sb->s_fs_info;
	vfs_entry_tree *tree = node->tree;
	if (node->sb == sb)
		node->sb = NULL;
	else
		sb_put(node->sb);
	free(sb);
	vfs_entry_tree_put(tree);
}

static int entry_mkdir(super_block *sb, const char *path, unsigned mode)
{
	vfs_entry_node *parent = sb->s_fs_info;
	struct stat metadata = { .st_mode = parent->mode };
	int error = fs_check_perm(&metadata, W_OK | X_OK);
	if (error)
		return error;
	if (parent->tree->magic != 0x1373)
		return -EROFS;
	if (!path || path[0] != '/' || !path[1] || strchr(path + 1, '/'))
		return -EINVAL;
	if (vfs_entry_child(parent, path + 1))
		return -EEXIST;
	return vfs_entry_directory_mode(parent, path + 1, mode) ?
		       0 :
		       parent->tree->error;
}

static int entry_statfs(super_block *sb, struct statfs64 *buf)
{
	vfs_entry_node *node = sb->s_fs_info;
	if (node->provider_ops && node->provider_ops->statfs)
		return node->provider_ops->statfs(sb, buf);
	memset(buf, 0, sizeof(*buf));
	buf->f_type = node->tree->magic;
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = 255;
	return 0;
}

static const super_operations entry_sops = {
	.open = entry_open,
	.open_root = entry_open_root,
	.readlink = vfs_entry_readlink,
	.release = entry_release_super,
	.mkdir = entry_mkdir,
	.statfs = entry_statfs,
};

vfs_entry_tree *vfs_entry_tree_create(void)
{
	vfs_entry_tree *tree = zalloc(sizeof(*tree));
	if (!tree)
		return NULL;
	tree->references = 1;
	mutex_init(&tree->lock);
	list_init(&tree->nodes);
	list_init(&tree->allocations);
	tree->root = vfs_entry_new_node(tree, NULL, "", S_IFDIR | 0555, 0);
	if (!tree->root) {
		vfs_entry_tree_put(tree);
		return NULL;
	}
	return tree;
}

super_block *vfs_entry_tree_super(vfs_entry_tree *tree)
{
	return tree->root->sb;
}

int vfs_entry_tree_error(vfs_entry_tree *tree)
{
	return tree->error;
}

void *vfs_entry_tree_alloc(vfs_entry_tree *tree, unsigned size)
{
	struct entry_allocation *allocation =
		zalloc(sizeof(*allocation) + size);
	if (!allocation) {
		tree->error = -ENOMEM;
		return NULL;
	}
	mutex_lock(&tree->lock);
	list_insert_head(&tree->allocations, &allocation->list);
	mutex_unlock(&tree->lock);
	return allocation + 1;
}

const char *vfs_entry_name(vfs_entry_node *node)
{
	return node->name;
}

/* The caller owns the detached superblock until vfs_mount() succeeds. */
vfs_entry_node *vfs_entry_directory_create(vfs_entry_node *parent,
					   const char *name)
{
	if (!parent)
		return NULL;
	return vfs_entry_new_node(parent->tree, parent, name, S_IFDIR | 0555,
				  0);
}

super_block *vfs_entry_super(vfs_entry_node *node)
{
	return node ? node->sb : NULL;
}

/* A mount owns its root and references the registered child superblocks. */
super_block *vfs_entry_tree_mount(vfs_entry_tree *tree)
{
	super_block *sb = sget(&entry_sops);
	struct rb_node *cursor;
	if (!sb)
		return NULL;
	sb->s_fs_info = tree->root;
	__sync_add_and_fetch(&tree->references, 1);
	sb_get(tree->root->sb);
	for (cursor = rb_first(&tree->root->sb->s_mounts); cursor;
	     cursor = rb_next(cursor)) {
		vfs_mount_node *mount =
			rb_entry(cursor, vfs_mount_node, rb_node);
		sb_get(mount->sb);
		if (vfs_mount(sb, mount->path, mount->sb)) {
			sb_put(mount->sb);
			sb_put(sb);
			return NULL;
		}
	}
	return sb;
}

vfs_entry_node *vfs_entry_directory_mode(vfs_entry_node *parent,
					 const char *name, unsigned mode)
{
	vfs_entry_node *node = vfs_entry_directory(parent, name);
	if (node)
		node->mode = S_IFDIR | (mode & 0777);
	return node;
}

vfs_entry_node *vfs_entry_directory_path(vfs_entry_node *parent, const char *path,
				       unsigned mode)
{
	char *component = malloc(256);
	if (!component || !path) { free(component); return NULL; }
	while (*path) {
		unsigned length = 0;
		while (*path == '/') path++;
		if (!*path) break;
		while (*path && *path != '/') {
			if (length == 255) { free(component); return NULL; }
			component[length++] = *path++;
		}
		component[length] = 0;
		parent = vfs_entry_directory_mode(parent, component, mode);
		if (!parent) break;
	}
	free(component);
	return parent;
}

vfs_entry_node *vfs_entry_mount(vfs_entry_node *parent, const char *name,
				unsigned mode, super_block *provider)
{
	vfs_entry_node *node;
	if (!parent || !provider)
		return NULL;
	node = vfs_entry_new_node(parent->tree, parent, name, mode, 1);
	if (node)
		node->provider = provider;
	return node;
}

void vfs_entry_set_provider(vfs_entry_node *node, const super_operations *ops)
{
	if (node)
		node->provider_ops = ops;
}

void vfs_entry_tree_type(vfs_entry_tree *tree, unsigned magic)
{
	if (tree)
		tree->magic = magic;
}

/* The generic node backend is also used by ordinary filesystem mknod. */

vfs_entry_node *vfs_entry_device(vfs_entry_node *parent, const char *path,
				 unsigned mode, unsigned devno,
				 const char *class_name,
				 file *(*open)(super_block *, unsigned, int))
{
	char *copy, *leaf;
	vfs_entry_node *node;
	super_block *provider;
	if (!parent || !path || !*path)
		return NULL;
	while (*path == '/')
		path++;
	copy = strdup(path);
	if (!copy)
		return NULL;
	leaf = copy;
	for (char *cursor = copy; *cursor; cursor++) {
		if (*cursor != '/')
			continue;
		*cursor = 0;
		parent = vfs_entry_directory(parent, leaf);
		leaf = cursor + 1;
		if (!parent) {
			free(copy);
			return NULL;
		}
	}
	provider = devnode_create(mode, devno);
	node = vfs_entry_mount(parent, leaf, mode, provider);
	free(copy);
	if (!node) {
		if (provider)
			sb_put(provider);
		return NULL;
	}
	if (S_ISCHR(mode) || S_ISBLK(mode)) {
		int error =
			S_ISCHR(mode) ?
				chardev_register(devno, class_name, open) :
				blockdev_register_node(devno, class_name, open);
		if (error) {
			parent->tree->error = error;
			return NULL;
		}
	}
	return node;
}

void vfs_entry_remove(vfs_entry_node *parent, const char *path)
{
	if (parent && path) {
		char *canonical = malloc(strlen(path) + 2);
		if (!canonical)
			return;
		if (*path == '/')
			strcpy(canonical, path);
		else
			sprintf(canonical, "/%s", path);
		vfs_umount(parent->sb, canonical);
		free(canonical);
	}
}

void vfs_entry_set_mode(vfs_entry_node *node, unsigned mode)
{
	if (node)
		node->mode = mode;
}
