#include <sys/sys.h>
#include <fs/mount.h>
#include <macro.h>
#include <hw/pci.h>
#include <hw/driver.h>
#include <lib/klib.h>

static vfs_entry_tree *sys_tree;

vfs_entry_tree *sys_entries(void)
{
	return sys_tree;
}

static super_block *sysfs_get_sb(const char *dev, const char *target, int flags,
				 void *data)
{
	if (!sys_tree || vfs_entry_tree_error(sys_tree))
		return NULL;
	return vfs_entry_tree_mount(sys_tree);
}

static fs_type sysfs_type = { .name = "sysfs", .get_sb = sysfs_get_sb };

struct pci_registration {
	super_block *devices;
	unsigned boot_vga;
};

static void sysfs_register_pci(unsigned address, uint16_t vendor, uint16_t id,
			       void *data)
{
	struct pci_registration *registration = data;
	vfs_entry_node *node;
	unsigned boot_vga = 0;
	char path[32];
	int error;
	if (!registration->boot_vga &&
	    pci_read_field(address, PCI_CLASS, 1) == 3 &&
	    pci_read_field(address, PCI_SUBCLASS, 1) == 0)
		boot_vga = registration->boot_vga = 1;
	if (!registration->devices)
		goto probe;
	node = sys_pci_create(address, boot_vga);
	if (!node) {
		printk("pci: cannot create sys entries for %x\n", address);
		goto probe;
	}
	sprintf(path, "/%s", vfs_entry_name(node));
	error = vfs_mount(registration->devices, path, vfs_entry_super(node));
	if (error) {
		sb_put(vfs_entry_super(node));
		printk("pci: cannot mount %s (%d)\n", path, error);
	}
probe:
	hw_probe_pci(address, vendor, id);
}

static void sysfs_register(void)
{
	struct pci_registration registration = { 0 };
	sys_tree = vfs_entry_tree_create();
	if (sys_tree) {
		registration.devices = vfs_entry_super(sys_pci_devices());
		fs_register_type(&sysfs_type);
	}
	pci_scan(sysfs_register_pci, PCI_SCAN_ALL, &registration);
}

/* PCI drivers register at level 4; device nodes initialize at level 6. */
KERNEL_INIT(5, sysfs_register);
