#include <config.h>
#include <errno.h>
#include <fs/entries.h>
#include <fs/inotify.h>
#include <lib/klib.h>
#include <proc/proc.h>
#include <ps/ps.h>

static int limit_show(void *data, unsigned tag, char *buffer, unsigned capacity)
{
	if (capacity < 12)
		return -ENOSPC;
	return sprintf(buffer, "%u\n", inotify_limit_get(tag));
}

static int space(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static ssize_t limit_write(void *data, unsigned tag, const void *buffer,
			   size_t size, loff_t *position)
{
	char text[32];
	unsigned value = 0, index = 0, digits = 0;
	int result;
	if (!size)
		return 0;
	if (size >= sizeof(text))
		return -EINVAL;
	if (ps_read_process_memory(current, buffer, text, size) < 0)
		return -EFAULT;
	while (index < size && space(text[index]))
		index++;
	if (index < size && text[index] == '+')
		index++;
	while (index < size && text[index] >= '0' && text[index] <= '9') {
		unsigned digit = text[index++] - '0';
		if (value > (0x7fffffffu - digit) / 10)
			return -EINVAL;
		value = value * 10 + digit;
		digits++;
	}
	while (index < size && space(text[index]))
		index++;
	if (!digits || index != size)
		return -EINVAL;
	result = inotify_limit_set(tag, value);
	if (result)
		return result;
	*position += size;
	return size;
}

static const vfs_entry_attribute_ops limit_ops = {
	.show = limit_show,
	.write = limit_write,
};

static void proc_sys_register(super_block *proc_sb)
{
	vfs_entry_tree *tree = vfs_entry_tree_create();
	vfs_entry_node *root, *kernel, *fs, *inotify;
	if (!tree)
		return;
	root = vfs_entry_root(tree);
	kernel = vfs_entry_directory(root, "kernel");
	vfs_entry_text(kernel, "osrelease", UTS_RELEASE "\n");
	fs = vfs_entry_directory(root, "fs");
	inotify = vfs_entry_directory(fs, "inotify");
	vfs_entry_attribute(inotify, "max_user_watches", 0644, &limit_ops, NULL,
			    INOTIFY_MAX_USER_WATCHES);
	vfs_entry_attribute(inotify, "max_user_instances", 0644, &limit_ops,
			    NULL, INOTIFY_MAX_USER_INSTANCES);
	vfs_entry_attribute(inotify, "max_queued_events", 0644, &limit_ops,
			    NULL, INOTIFY_MAX_QUEUED_EVENTS);
	if (vfs_entry_tree_error(tree) ||
	    vfs_mount(proc_sb, "/sys", vfs_entry_tree_super(tree)))
		sb_put(vfs_entry_tree_super(tree));
}
PROC_INIT(proc_sys_register);
