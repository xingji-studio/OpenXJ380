#include "nvme/nvme.h"
#include <pci/pci.h>

int nvme_probe(pci_device_t *dev, uint32_t vendor_device_id)
{
    write_serial_fmt("Found NVME controller.\n");

    NVME_CONTROLLER *nvme = nvme_driver_init(dev->bars[0].address, dev->bars[0].size);
    if (!nvme)
        return -1;

    // dev->desc = nvme;

    return 0;
}

void nvme_remove(pci_device_t *dev)
{
}

void nvme_shutdown(pci_device_t *dev)
{
}

void nvme_setup()
{
    pci_device_t * nvdev = pci_find_class(0x010800);
    if(nvdev==NULL)return;
    nvme_probe(nvdev,NULL);
}
