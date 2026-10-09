#include <driver/driver.h>
#include <fs/entries.h>
#include <proc/proc.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <errno.h>

/* /dev/fd is a descriptor directory for the calling process. */
static file *dev_fd_open_root(super_block *sb, int flag)
{
	return proc_pid_lookup(CURRENT_TASK()->life->psid, "/fd", flag);
}

static file *dev_fd_open(super_block *sb, const char *path, int flag)
{
	char *name = name_get();
	file *fp;
	if (!name || strlen(path) + 4 >= MAX_PATH) {
		if (name)
			name_put(name);
		return NULL;
	}
	sprintf(name, "/fd%s", path);
	fp = proc_pid_lookup(CURRENT_TASK()->life->psid, name, flag);
	name_put(name);
	return fp;
}

static int dev_fd_readlink(super_block *sb, const char *path, char *buf,
			   size_t bufsiz, size_t *rcnt)
{
	char *name = name_get();
	int ret;
	if (!name || strlen(path) + sizeof("/proc/self/fd") > MAX_PATH) {
		if (name)
			name_put(name);
		return -ENOENT;
	}
	sprintf(name, "/proc/self/fd%s", path);
	ret = vfs_readlink(CURRENT_TASK()->fs->root, name, buf, bufsiz, rcnt);
	name_put(name);
	return ret;
}

static const super_operations dev_fd_sops = {
	.open_root = dev_fd_open_root,
	.open = dev_fd_open,
	.readlink = dev_fd_readlink,
	.release = NULL,
};

static int descriptor_directory_probe(void)
{
	super_block *provider = sget(&dev_fd_sops);
	if (!provider)
		return -ENOMEM;
	if (!vfs_entry_mount(devfs_entries(), "fd", S_IFDIR | 0555, provider)) {
		sb_put(provider);
		return -ENOMEM;
	}
	return 0;
}

static driver_t descriptor_directory_driver = {
	.name = "file-descriptors",
	.bus = DEVICE_BUS_VIRTUAL,
	.virtual_id = VDEV_FD,
	.probe_virtual = descriptor_directory_probe,
};
DRIVER_REGISTER(descriptor_directory_driver);
