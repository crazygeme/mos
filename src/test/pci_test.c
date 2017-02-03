#include <pci.h>
#include <pci_list.h>


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

static char* pci_get_ven_name(uint16_t v, uint16_t d)
{
    int i = 0;
    for (i = 0; i < PCI_VENTABLE_LEN; i++){
        if (PciVenTable[i].VenId == v){
            return PciVenTable[i].VenFull;
        }
    }
    return "";
}

static void scan_all_pci(uint32_t device, uint16_t v, uint16_t d, void * extra)
{
    printf("PCI: VEN_%x&DEV_%x, vendor %s, chip %s\n", 
            v,
            d,
            pci_get_ven_name(v, d),
            pci_get_chip_name(v,d));
}

void test_pci()
{
    pci_scan(scan_all_pci, PCI_SCAN_ALL, 0);
}