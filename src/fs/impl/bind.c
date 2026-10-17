#include <fs/vfs.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <errno.h>
#include <macro.h>
struct bind_info {
	super_block *source;
	file *held;
	char *prefix;
};
static char *bind_path(struct bind_info *info, const char *path)
{
	unsigned n = strlen(info->prefix), m = strlen(path);
	if (n + m + 1 >= MAX_PATH)
		return NULL;
	char *out = name_get();
	if (!out)
		return NULL;
	strcpy(out, info->prefix);
	if (!strcmp(out, "/") && path[0] == '/')
		out[0] = 0;
	strcat(out, path);
	return out;
}
static file *bind_file_path(file *fp, super_block *sb, const char *path)
{
	if (!fp)
		return NULL;
	char *name = malloc(strlen(sb->s_mountpoint) + strlen(path) + 2);
	if (!name) {
		fs_put_file(fp);
		return NULL;
	}
	strcpy(name, sb->s_mountpoint);
	if (!strcmp(name, "/") && path[0] == '/')
		name[0] = 0;
	strcat(name, path);
	if (fp->f_name && !fp->f_name_inline)
		free(fp->f_name);
	fp->f_name = name;
	fp->f_name_inline = 0;
	if (fp->f_sb && fp->f_sb != sb) {
		sb_put(fp->f_sb);
		fp->f_sb = NULL;
	}
	vfs_set_file_origin(fp, sb, path);
	return fp;
}

static file *bind_open(super_block *sb, const char *path, int flags)
{
	struct bind_info *info = sb->s_fs_info;
	if (info->held)
		return !*path ? bind_file_path(info->held->f_fop->reopen(
						       info->held, flags),
					       sb, path) :
				NULL;
	char *full = bind_path(info, path);
	if (!full)
		return NULL;
	file *fp = (!*full || !strcmp(full, "/")) &&
				   info->source->s_op->open_root ?
			   info->source->s_op->open_root(info->source, flags) :
		   info->source->s_op->open ?
			   info->source->s_op->open(info->source, full, flags) :
			   NULL;
	name_put(full);
	return bind_file_path(fp, sb, path);
}
static file *bind_open_root(super_block *sb, int flags)
{
	return bind_open(sb, "", flags);
}
static int bind_resolve_link(super_block *sb, const char *path, char **target)
{
	struct bind_info *info = sb->s_fs_info;
	if (!info->source->s_op->readlink)
		return 0;
	char *full = bind_path(info, path);
	char *text = name_get(), *link = name_get();
	if (!full || !text || !link) {
		if (full) name_put(full);
		if (text) name_put(text);
		if (link) name_put(link);
		return -ENOMEM;
	}
	size_t prefix = strlen(full) - strlen(path);
	int result = 0;
	for (size_t i = 1; path[i - 1]; i++) {
		if (path[i] && path[i] != '/')
			continue;
		char saved = full[prefix + i];
		full[prefix + i] = 0;
		size_t length = 0;
		int ret = info->source->s_op->readlink(info->source, full,
						      text, MAX_PATH - 1, &length);
		full[prefix + i] = saved;
		if (ret || !length)
			continue;
		text[length] = 0;
		const char *base = !strcmp(sb->s_mountpoint, "/") ? "" : sb->s_mountpoint;
		if (strlen(base) + i >= MAX_PATH) { result = -ENAMETOOLONG; break; }
		strcpy(link, base);
		memcpy(link + strlen(base), path, i);
		link[strlen(base) + i] = 0;
		ret = fs_resolve_symlink_path(link, text, length);
		if (ret || strlen(text) + strlen(path + i) >= MAX_PATH) {
			result = ret ? ret : -ENAMETOOLONG; break;
		}
		strcat(text, path + i);
		*target = text;
		text = NULL;
		result = 1;
		break;
	}
	name_put(full);
	name_put(link);
	if (text) name_put(text);
	return result;
}
static file *bind_open_link(super_block *sb, const char *path, int flags,
			    char **target)
{
	int ret = bind_resolve_link(sb, path, target);
	if (ret)
		return NULL;
	return bind_open(sb, path, flags);
}

static int bind_mkdir(super_block *sb, const char *path, unsigned mode)
{
	struct bind_info *info = sb->s_fs_info;
	char *full = bind_path(info, path);
	if (!full)
		return -ENAMETOOLONG;
	int ret = info->source->s_op->mkdir ?
			  info->source->s_op->mkdir(info->source, full, mode) :
			  -EOPNOTSUPP;
	name_put(full);
	return ret;
}
static int bind_rmdir(super_block *sb, const char *path)
{
	struct bind_info *info = sb->s_fs_info;
	char *full = bind_path(info, path);
	if (!full)
		return -ENAMETOOLONG;
	int ret = info->source->s_op->rmdir ?
			  info->source->s_op->rmdir(info->source, full) :
			  -EOPNOTSUPP;
	name_put(full);
	return ret;
}
static int bind_unlink(super_block *sb, const char *path)
{
	struct bind_info *info = sb->s_fs_info;
	char *full = bind_path(info, path);
	if (!full)
		return -ENAMETOOLONG;
	int ret = info->source->s_op->unlink ?
			  info->source->s_op->unlink(info->source, full) :
			  -EOPNOTSUPP;
	name_put(full);
	return ret;
}
static int bind_utime(super_block *sb, const char *path, unsigned atime,
		      unsigned mtime)
{
	struct bind_info *info = sb->s_fs_info;
	char *full = bind_path(info, path);
	if (!full)
		return -ENAMETOOLONG;
	int ret = info->source->s_op->utime ?
			  info->source->s_op->utime(info->source, full, atime,
						    mtime) :
			  -EOPNOTSUPP;
	name_put(full);
	return ret;
}
static int bind_readlink(super_block *sb, const char *path, char *buf,
			 size_t size, size_t *length)
{
	struct bind_info *info = sb->s_fs_info;
	char *full = bind_path(info, path);
	if (!full)
		return -ENAMETOOLONG;
	int ret = info->source->s_op->readlink ?
			  info->source->s_op->readlink(info->source, full, buf,
						       size, length) :
			  -EOPNOTSUPP;
	name_put(full);
	return ret;
}
static int bind_link(super_block *sb, const char *oldpath, const char *newpath)
{
	struct bind_info *info = sb->s_fs_info;
	char *old = bind_path(info, oldpath), *new = bind_path(info, newpath);
	int ret = -ENAMETOOLONG;
	if (old && new)
		ret = info->source->s_op->link ?
			      info->source->s_op->link(info->source, old, new) :
			      -EOPNOTSUPP;
	if (old)
		name_put(old);
	if (new)
		name_put(new);
	return ret;
}
static int bind_rename(super_block *sb, const char *oldpath,
		       const char *newpath)
{
	struct bind_info *info = sb->s_fs_info;
	char *old = bind_path(info, oldpath), *new = bind_path(info, newpath);
	int ret = -ENAMETOOLONG;
	if (old && new)
		ret = info->source->s_op->rename ?
			      info->source->s_op->rename(info->source, old,
							 new) :
			      -EOPNOTSUPP;
	if (old)
		name_put(old);
	if (new)
		name_put(new);
	return ret;
}
static int bind_symlink(super_block *sb, const char *target, const char *path)
{
	struct bind_info *info = sb->s_fs_info;
	char *full = bind_path(info, path);
	if (!full)
		return -ENAMETOOLONG;
	int ret = info->source->s_op->symlink ?
			  info->source->s_op->symlink(info->source, target,
						      full) :
			  -EOPNOTSUPP;
	name_put(full);
	return ret;
}
static int bind_statfs(super_block *sb, struct statfs64 *out)
{
	struct bind_info *info = sb->s_fs_info;
	return info->source->s_op->statfs ?
		       info->source->s_op->statfs(info->source, out) :
		       -EOPNOTSUPP;
}
static void bind_release(super_block *sb)
{
	struct bind_info *info = sb->s_fs_info;
	if (info->held)
		fs_put_file(info->held);
	sb_put(info->source);
	free(info->prefix);
	free(info);
	free(sb);
}
static const super_operations bind_sops = {
	.open = bind_open,
	.open_root = bind_open_root,
	.open_link = bind_open_link,
	.mkdir = bind_mkdir,
	.rmdir = bind_rmdir,
	.unlink = bind_unlink,
	.link = bind_link,
	.rename = bind_rename,
	.symlink = bind_symlink,
	.readlink = bind_readlink,
	.utime = bind_utime,
	.statfs = bind_statfs,
	.release = bind_release,
};
super_block *vfs_bind_mount(super_block *root, const char *path, int recursive)
{
	super_block *source;
	char *suffix;
	if (!sb_path_resolve(root, path, &source, &suffix))
		return NULL;
	if (!strcmp(suffix, "/"))
		suffix++;
	if (source->s_unbindable) return NULL;
	super_block *sb = sget(&bind_sops);
	struct bind_info *info = zalloc(sizeof(*info));
	if (!sb || !info) {
		free(sb);
		free(info);
		return NULL;
	}
	info->source = source->s_backend ? source->s_backend : source;
	sb_get(info->source);
	info->prefix = strdup(suffix);
	if (!info->prefix) {
		sb_put(info->source);
		free(info);
		free(sb);
		return NULL;
	}
	sb->s_fs_info = info;
	sb->s_flags = source->s_flags;
	sb->s_peer_group = source->s_peer_group;
	sb->s_master_group = source->s_master_group;
	strcpy(sb->s_fstype, source->s_fstype[0] ? source->s_fstype : "none");
	strcpy(sb->s_devname, source->s_devname);
	file *opened = vfs_open(root, path, O_RDONLY);
	if (opened && opened->f_fop && opened->f_fop->reopen &&
	    !S_ISDIR(opened->f_inode->i_mode)) {
		info->held = opened;
		strcpy(sb->s_fstype, "nsfs");
	} else if (opened)
		fs_put_file(opened);
	if (recursive) {
		unsigned len = strlen(suffix);
		mutex_lock(&source->s_lock);
		for (struct rb_node *n = rb_first(&source->s_mounts); n;
		     n = rb_next(n)) {
			vfs_mount_node *mount =
				rb_entry(n, vfs_mount_node, rb_node);
			if (mount->sb->s_unbindable) continue;
			if (strncmp(mount->path, suffix, len) ||
			    (len && mount->path[len] != '/'))
				continue;
			super_block *copy = vfs_clone_bind_tree(mount->sb);
			if (!copy || vfs_mount(sb, mount->path + len, copy)) {
				if (copy)
					sb_put(copy);
				mutex_unlock(&source->s_lock);
				sb_put(sb);
				return NULL;
			}
		}
		mutex_unlock(&source->s_lock);
	}
	return sb;
}
