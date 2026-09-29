#include <sys/sys.h>
#include <dev/virtgpu.h>
#include <lib/klib.h>
#include <errno.h>

int sys_drm_register(unsigned address)
{
	static const struct {
		const char *name;
		unsigned minor;
	} nodes[] = {
		{ "card0", 0 },
		{ "renderD128", GPU_RENDER_MINOR },
	};
	vfs_entry_node *device, *devices, *class, *characters;
	char *text;
	unsigned i;
	vfs_entry_tree *tree = sys_entries();
	if (!tree)
		return -ENODEV;
	device = sys_pci_device(address);
	if (!device)
		return -ENODEV;
	devices = vfs_entry_directory(device, "drm");
	class = vfs_entry_directory(
		vfs_entry_directory(vfs_entry_root(tree), "class"), "drm");
	characters = vfs_entry_directory(
		vfs_entry_directory(vfs_entry_root(tree), "dev"), "char");
	if (!devices || !class || !characters)
		return -ENOMEM;
	text = name_get();
	if (!text)
		return -ENOMEM;
	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		vfs_entry_node *node =
			vfs_entry_directory(devices, nodes[i].name);
		if (!node || !vfs_entry_link(class, nodes[i].name, node) ||
		    !vfs_entry_link(node, "device", device) ||
		    !vfs_entry_link(node, "subsystem", class))
			goto fail;
		sprintf(text, "%u:%u", GPU_MAJOR, nodes[i].minor);
		if (!vfs_entry_link(characters, text, node))
			goto fail;
		sprintf(text, "%u:%u\n", GPU_MAJOR, nodes[i].minor);
		if (!vfs_entry_text(node, "dev", text))
			goto fail;
		sprintf(text, "MAJOR=%u\nMINOR=%u\nDEVNAME=dri/%s\n", GPU_MAJOR,
			nodes[i].minor, nodes[i].name);
		if (!vfs_entry_text(node, "uevent", text))
			goto fail;
	}
	name_put(text);
	return 0;
fail:
	name_put(text);
	return -ENOMEM;
}
