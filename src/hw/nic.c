#include <nic.h>
#include <klib.h>
#include <pci_list.h>

static nic_dev network_devices[MAX_NETWORK_DEV] = {0};

static void nic_register(nic_dev* dev)
{
    int i = 0;
    if (dev == NULL)
        return;

    for (i = 0; i < MAX_NETWORK_DEV; i++){
        if(network_devices[i].ven == 0 &&
           network_devices[i].dev == 0){
               network_devices[i] = *dev;
               return;
           }
    }
}

static char* pci_get_chip_name(uint16_t v, uint16_t d)
{
    int i = 0;
    for (i = 0; i < PCI_DEVTABLE_LEN; i++){
        if (PciDevTable[i].VenId == v && PciDevTable[i].DevId == d){
            return PciDevTable[i].Chip;
        }
    }
    return "";
}

static void scan_all_pci(uint32_t device, uint16_t v, uint16_t d, void * extra)
{
    char* chip;
    nic_dev* dev = NULL;
    int ret;
    chip = pci_get_chip_name(v, d);
    if (!chip) // unknown chip
        return;

    if (v == 0x8086){ // intel
        if (memcmp(chip, "8254", 4) == 0)
            dev = nic_intel_8254x_create(device, v, d);
        // TODO: other chip
    }else{
        // TODO: other vendor
    }

    if(dev){
        if (!dev->init){
            free(dev);
            return;
        }

        if (dev->init(dev) != 0){
            free(dev);
            return;
        }
        
        nic_register(dev);
        free(dev);
    }
}

void nic_scan_all()
{
     pci_scan(scan_all_pci, PCI_SCAN_ALL, 0);
}

nic_dev* nic_getdev_by_mac(uint8_t* mac)
{
    int i = 0;
    for (i = 0; i < MAX_NETWORK_DEV; i++){
        if(memcmp(network_devices[i].mac_addr, mac, 6) == 0){
               return &network_devices[i];
           }
    }
    return NULL;
}

nic_dev* nic_getdev_by_ip(uint8_t* ip)
{
    int i = 0;
    for (i = 0; i < MAX_NETWORK_DEV; i++){
        if(memcmp(network_devices[i].ip_addr, ip, 6) == 0){
               return &network_devices[i];
           }
    }
    return NULL;
}