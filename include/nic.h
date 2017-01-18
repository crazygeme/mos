#ifndef NIC_H
#define NIC_H
#include <pci.h>

#define MAX_NETWORK_DEV 12

typedef struct _nic_dev
{
    uint32_t pci_dev;
    uint16_t ven;
    uint16_t dev;
    uint8_t mac_addr[6];
    uint8_t ip_addr[6];
    int (*init)(void* dev);
    int (*uninit)(void* dev);
    int (*up)(void* dev);
    int (*down)(void* dev);
    void* ctx;
}nic_dev;

void nic_scan_all();

nic_dev* nic_getdev_by_mac(uint8_t* mac);

nic_dev* nic_getdev_by_ip(uint8_t* ip);

#endif