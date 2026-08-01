#include "ahci/ahci.h"
#include "proto.hpp"

void scsi_create_packet12(struct scsi_cdb12 *cdb, uint8_t opcode, uint32_t lba, uint32_t alloc_size)
{
    memset(cdb, 0, sizeof(struct scsi_cdb12));
    cdb->opcode = opcode;
    cdb->lba_be = SCSI_FLIP(lba);
    cdb->length = SCSI_FLIP(alloc_size);
}

void scsi_create_packet16(struct scsi_cdb16 *cdb, uint8_t opcode, uint64_t lba, uint32_t alloc_size)
{
    memset(cdb, 0, sizeof(struct scsi_cdb16));
    cdb->opcode    = opcode;
    cdb->lba_be_hi = SCSI_FLIP((uint32_t)(lba >> 32));
    cdb->lba_be_lo = SCSI_FLIP((uint32_t)lba);
    cdb->length    = SCSI_FLIP(alloc_size);
}

void scsi_parse_capacity(struct hba_device *device, uint32_t *parameter)
{
    if (device->cbd_size == SCSI_CDB16)
    {
        device->max_lba    = (uint64_t)SCSI_FLIP(*(parameter + 1)) | ((uint64_t)SCSI_FLIP(*parameter) << 32);
        device->block_size = SCSI_FLIP(*(parameter + 2));
    }
    else
    {
        // for READ_CAPACITY(10)
        device->max_lba    = SCSI_FLIP(*(parameter));
        device->block_size = SCSI_FLIP(*(parameter + 1));
    }
}

void scsi_submit(struct hba_device *dev, struct blkio_req *io_req)
{
    struct hba_port *port = dev->port;
    struct hba_cmdh *header;
    struct hba_cmdt *table;
    io_req->status        = -1;

    int write = !!(io_req->flags & BLKIO_WRITE);
    int slot  = hba_prepare_cmd(port, &table, &header);
    if (slot < 0) {
        write_serial_fmt("AHCI: No free command slot for ATAPI %s lba=%llu len=%llu\n", write ? "write" : "read",
                         io_req->lba, io_req->len);
        return;
    }
    if (hba_bind_sbuf(header, table, (void *)io_req->buf, io_req->len) != 0) {
        write_serial_fmt("AHCI: Failed to bind ATAPI %s buffer lba=%llu len=%llu\n", write ? "write" : "read",
                         io_req->lba, io_req->len);
        return;
    }

    header->options |= HBA_CMDH_ATAPI | (HBA_CMDH_WRITE * write);

    size_t   size  = io_req->len;
    uint32_t count = ICEIL(size, port->device->block_size);

    struct sata_reg_fis *fis = (struct sata_reg_fis *)(&table->command_fis);
    void                *cdb = table->atapi_cmd;
    sata_create_fis(fis, ATA_PACKET, (size << 8), 0);
    fis->feature = 1 | ((!write) << 2);

    if (port->device->cbd_size == SCSI_CDB16)
    {
        scsi_create_packet16((struct scsi_cdb16 *)cdb, write ? SCSI_WRITE_BLOCKS_16 : SCSI_READ_BLOCKS_16, io_req->lba,
                             count);
    }
    else
    {
        scsi_create_packet12((struct scsi_cdb12 *)cdb, write ? SCSI_WRITE_BLOCKS_12 : SCSI_READ_BLOCKS_12,
                             io_req->lba & (uint32_t)-1, count);
    }

    // field: cdb->misc1
    *((uint8_t *)cdb + 1) = 3 << 5; // RPROTECT=011b 禁用保护检�?
    if (ahci_try_send(port, slot)) {
        io_req->status = 0;
    }
}
