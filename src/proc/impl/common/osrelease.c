#include <config.h>
#include <errno.h>
#include <fs/entries.h>
#include <fs/inotify.h>
#include <lib/klib.h>
#include <proc/proc.h>
#include <ps/ps.h>
#include <ps/namespace.h>

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

static int uts_show(void *data, unsigned tag, char *buffer, unsigned capacity)
{
	(void)data;
	const char *value = tag ? ps_namespace_domainname(current) :
				ps_namespace_hostname(current);
	if (strlen(value) + 2 > capacity)
		return -ENOSPC;
	return sprintf(buffer, "%s\n", value);
}
static ssize_t uts_write(void *data, unsigned tag, const void *buffer,
			 size_t size, loff_t *position)
{
	(void)data;
	char text[65];
	if (!ps_has_capability(current, 21))
		return -EPERM;
	if (size > 65)
		return -EINVAL;
	if (ps_read_process_memory(current, buffer, text, size) < 0)
		return -EFAULT;
	unsigned length = size;
	if (length && text[length - 1] == '\n')
		length--;
	if (length > 64)
		return -EINVAL;
	text[length] = 0;
	strcpy(tag ? ps_namespace_domainname(current) : ps_namespace_hostname(current), text);
	*position += size;
	return size;
}
static const vfs_entry_attribute_ops uts_ops = { .show = uts_show, .write = uts_write };

static void proc_sys_register(void)
{
	vfs_entry_node *root = procfs_entries();
	if (!root)
		return;
	vfs_entry_node *kernel, *fs, *inotify;
	root = vfs_entry_directory(root, "sys");
	kernel = vfs_entry_directory(root, "kernel");
	vfs_entry_text(kernel, "osrelease", UTS_RELEASE "\n");
	vfs_entry_text(kernel, "cap_last_cap", "40\n");
	vfs_entry_attribute(kernel, "hostname", 0644, &uts_ops, NULL, 0);
	vfs_entry_attribute(kernel, "domainname", 0644, &uts_ops, NULL, 1);
	unsigned char uuid[16];
	char boot_id[38];
	kernel_random_bytes(uuid, sizeof(uuid));
	uuid[6] = (uuid[6] & 15) | 0x40;
	uuid[8] = (uuid[8] & 63) | 0x80;
	sprintf(boot_id, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
		uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
		uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
	vfs_entry_text(vfs_entry_directory(kernel, "random"), "boot_id", boot_id);
	fs = vfs_entry_directory(root, "fs");
	inotify = vfs_entry_directory(fs, "inotify");
	vfs_entry_attribute(inotify, "max_user_watches", 0644, &limit_ops, NULL,
			    INOTIFY_MAX_USER_WATCHES);
	vfs_entry_attribute(inotify, "max_user_instances", 0644, &limit_ops,
			    NULL, INOTIFY_MAX_USER_INSTANCES);
	vfs_entry_attribute(inotify, "max_queued_events", 0644, &limit_ops,
			    NULL, INOTIFY_MAX_QUEUED_EVENTS);
}
KERNEL_INIT(4, proc_sys_register);
