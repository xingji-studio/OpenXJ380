#include "ahci/ahci.h"
#include "proto.hpp"

extern bool ahci_get_accel();

static const size_t kAhciMaxBytesPerCmd = 0x400000UL;

static size_t ahci_max_sectors_per_cmd(const struct hba_device *dev)
{
    size_t limit = kAhciMaxBytesPerCmd / dev->block_size;
    if (limit == 0) { limit = 1; }
    if (limit > 0xffff) { limit = 0xffff; }
    return limit;
}

void sata_read_error(struct hba_port *port)
{
    write_serial_fmt("SATA read error\n");
    uint32_t tfd                        = port->regs[HBA_RPxTFD];
    port->device->last_result.sense_key = (tfd & 0xf000) >> 12;
    port->device->last_result.error     = (tfd & 0x0f00) >> 8;
    port->device->last_result.status    = tfd & 0x00ff;
}

static void sata_submit_safe(struct hba_device *dev, struct blkio_req *io_req)
{
    struct hba_port *port = dev->port;
    struct hba_cmdh *header;
    struct hba_cmdt *table;
    int              write = !!(io_req->flags & BLKIO_WRITE);
    int              slot  = hba_prepare_cmd(port, &table, &header);
    if (slot < 0) {
        write_serial_fmt("AHCI: No free command slot for %s lba=%llu len=%llu\n", write ? "write" : "read",
                         io_req->lba, io_req->len);
        return;
    }
    if (hba_bind_sbuf(header, table, (void *)io_req->buf, io_req->len) != 0) {
        write_serial_fmt("AHCI: Failed to bind %s buffer lba=%llu len=%llu\n", write ? "write" : "read", io_req->lba,
                         io_req->len);
        return;
    }
    if (!write && io_req->lba < 8) {
        uint64_t table_phys = (uint64_t)driver_virt_to_phys((uint64_t)table);
        uint64_t buf_phys   = (uint64_t)driver_virt_to_phys((uint64_t)io_req->buf);
        // write_serial_fmt("AHCI TRACE: lba=%llu len=0x%llx slot=%d table=0x%llx table_phys=0x%llx buf=0x%llx buf_phys=0x%llx prdt=0x%llx\n",
                        //  io_req->lba, io_req->len, slot, (uint64_t)table, table_phys, io_req->buf, buf_phys,
                        //  ((uint64_t)table->entries[0].data_base_upper << 32) | table->entries[0].data_base);
    }
    header->options |= HBA_CMDH_WRITE * write;
    uint16_t             count = ICEIL(io_req->len, port->device->block_size);
    struct sata_reg_fis *fis   = (struct sata_reg_fis *)(&table->command_fis);
    if ((port->device->flags & HBA_DEV_FEXTLBA))
    {
        sata_create_fis(fis, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, io_req->lba, count);
    }
    else { sata_create_fis(fis, write ? ATA_WRITE_DMA : ATA_READ_DMA, io_req->lba, count); }
    fis->dev = (1 << 6);
    if (!write && io_req->lba < 8) {
        // write_serial_fmt("AHCI TRACE: FIS=%02x %02x %02x %02x %02x %02x %02x %02x count=0x%x\n",
                        //  fis->head.type, fis->head.options, fis->head.status_cmd, fis->head.feat_err, fis->lba0,
                        //  fis->lba8, fis->lba16, fis->dev, fis->count);
    }
    if (ahci_try_send(port, slot)) {
        io_mfence();
        if (!write && io_req->lba < 8) {
            // write_serial_fmt("AHCI TRACE: done lba=%llu prdbc=0x%x tfd=0x%x\n",
                            //  io_req->lba, header->transferred_size, port->regs[HBA_RPxTFD]);
        }
        delay_us_hp(50);
        io_req->status = 0;
    }
}

static void sata_submit_fast(struct hba_device *dev, struct blkio_req *io_req)
{
    struct hba_port *port = dev->port;
    int              write = !!(io_req->flags & BLKIO_WRITE);

    if (io_req->len == 0) {
        io_req->status = 0;
        return;
    }

    uint64_t remaining = io_req->len;
    uint64_t offset    = 0;
    uint64_t lba       = io_req->lba;
    size_t   max_bytes = ahci_max_sectors_per_cmd(dev) * dev->block_size;

    while (remaining > 0) {
        struct hba_cmdh *header;
        struct hba_cmdt *table;
        uint32_t         chunk = remaining > max_bytes ? max_bytes : remaining;
        int              slot  = hba_prepare_cmd(port, &table, &header);
        if (slot < 0) {
            write_serial_fmt("AHCI: No free command slot for %s lba=%llu len=%llu\n", write ? "write" : "read", lba,
                             chunk);
            return;
        }
        if (hba_bind_sbuf(header, table, (void *)(io_req->buf + offset), chunk) != 0) {
            write_serial_fmt("AHCI: Failed to bind %s buffer lba=%llu len=%u\n", write ? "write" : "read", lba,
                             chunk);
            return;
        }
        if (!write && lba < 8) {
            uint64_t table_phys = (uint64_t)driver_virt_to_phys((uint64_t)table);
            uint64_t buf_phys   = (uint64_t)driver_virt_to_phys((uint64_t)(io_req->buf + offset));
            // write_serial_fmt("AHCI TRACE: lba=%llu len=0x%x slot=%d table=0x%llx table_phys=0x%llx buf=0x%llx buf_phys=0x%llx prdt=0x%llx\n",
                            //  lba, chunk, slot, (uint64_t)table, table_phys, io_req->buf + offset, buf_phys,
                            //  ((uint64_t)table->entries[0].data_base_upper << 32) | table->entries[0].data_base);
        }
        header->options |= HBA_CMDH_WRITE * write;
        uint16_t             count = ICEIL(chunk, port->device->block_size);
        struct sata_reg_fis *fis   = (struct sata_reg_fis *)(&table->command_fis);
        if ((port->device->flags & HBA_DEV_FEXTLBA))
        {
            sata_create_fis(fis, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, count);
        }
        else { sata_create_fis(fis, write ? ATA_WRITE_DMA : ATA_READ_DMA, lba, count); }
        fis->dev = (1 << 6);
        if (!write && lba < 8) {
            // write_serial_fmt("AHCI TRACE: FIS=%02x %02x %02x %02x %02x %02x %02x %02x count=0x%x\n",
                            //  fis->head.type, fis->head.options, fis->head.status_cmd, fis->head.feat_err, fis->lba0,
                            //  fis->lba8, fis->lba16, fis->dev, fis->count);
        }
        if (!ahci_try_send(port, slot)) { return; }
        io_mfence();
        if (!write && lba < 8) {
            // write_serial_fmt("AHCI TRACE: done lba=%llu prdbc=0x%x tfd=0x%x\n", lba, header->transferred_size,
                            //  port->regs[HBA_RPxTFD]);
        }
        remaining -= chunk;
        offset    += chunk;
        lba       += count;
    }

    io_req->status = 0;
}

void sata_submit(struct hba_device *dev, struct blkio_req *io_req)
{
    io_req->status = -1;
    if (ahci_get_accel()) {
        sata_submit_fast(dev, io_req);
    } else {
        sata_submit_safe(dev, io_req);
    }
}
