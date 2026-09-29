#ifndef MOS_DEVICE_PCI_SYSFS_H
#define MOS_DEVICE_PCI_SYSFS_H
#include <device/device.h>
#include <fs/entries.h>

void pci_sysfs_register(void);
vfs_entry_node *pci_sysfs_bus(void);
vfs_entry_node *pci_sysfs_devices(void);
vfs_entry_node *pci_sysfs_create(const device_t *device);
vfs_entry_node *pci_sysfs_hardware(void);
vfs_entry_node *pci_sysfs_drivers(void);
vfs_entry_node *pci_sysfs_device(unsigned address);
#endif
