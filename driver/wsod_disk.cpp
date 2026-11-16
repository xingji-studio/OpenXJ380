// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <wsod_storage.h>

int wsod_disk_prepare(const BlockExtent &extent, WsodDiskTarget *target)
{
    int error = ahci_wsod_prepare(extent, target);
    if (error == -EOPNOTSUPP) error = nvme_wsod_prepare(extent, target);
    if (error == -EOPNOTSUPP) error = ide_wsod_prepare(extent, target);
    return error;
}

int wsod_disk_write(WsodDiskTarget *target, uint64_t lba, const void *data, unsigned sectors)
{
    if (!target) return -EINVAL;
    switch (target->transport)
    {
    case 1: return ahci_wsod_write(target, lba, data, sectors);
    case 2: return nvme_wsod_write(target, lba, data, sectors);
    case 3: return ide_wsod_write(target, lba, data, sectors);
    default: return -ENODEV;
    }
}

int wsod_disk_flush(WsodDiskTarget *target)
{
    if (!target) return -EINVAL;
    switch (target->transport)
    {
    case 1: return ahci_wsod_flush(target);
    case 2: return nvme_wsod_flush(target);
    case 3: return ide_wsod_flush(target);
    default: return -ENODEV;
    }
}
