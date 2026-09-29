#ifndef MOS_SYS_SYS_H
#define MOS_SYS_SYS_H
#include <fs/entries.h>

vfs_entry_tree *sys_entries(void);
vfs_entry_node *sys_pci_bus(void);
vfs_entry_node *sys_pci_devices(void);
vfs_entry_node *sys_pci_create(unsigned address, unsigned boot_vga);
vfs_entry_node *sys_pci_device(unsigned address);
int sys_drm_register(unsigned address);
#endif
