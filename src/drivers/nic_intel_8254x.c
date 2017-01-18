#include <nic.h>
#include <klib.h>

static int nic_intel_8254x_init(nic_dev* dev)
{

}

static int nic_intel_8254x_uninit(nic_dev* dev)
{
    
}

static int nic_intel_8254x_up(nic_dev* dev)
{
    
}

static int nic_intel_8254x_down(nic_dev* dev)
{
    
}


nic_dev* nic_intel_8254x_create(uint32_t device, uint16_t v, uint16_t d)
{
    nic_dev* dev = malloc(sizeof(*dev));
    dev->pci_dev = device;
    dev->ven = v;
    dev->dev = d;
    memset(dev->mac_addr, 0, 6);
    memset(dev->ip_addr, 0, 6);
    dev->init = nic_intel_8254x_init;
    dev->uninit = nic_intel_8254x_uninit;
    dev->up = nic_intel_8254x_up;
    dev->down = nic_intel_8254x_down;
    dev->ctx = NULL;
    return dev;
}