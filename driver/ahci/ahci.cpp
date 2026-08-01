#include <ahci/ahci.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <device.h>
#include <ioctl.h>

void              *op_buffer;
static uint32_t    cdb_size[] = {SCSI_CDB12, SCSI_CDB16, 0, 0};
struct hba_device *hbaDevice[AHCI_MAX_DEVICES];
static bool        ahci_running_on_qemu = false;
static bool        ahci_accel_enabled   = false;

static bool ahci_port_link_ready(uint32_t ssts) {
    return HBA_PXSSTS_DET(ssts) == 0x3 && HBA_PXSSTS_IPM(ssts) == 0x1;
}

static void ahci_log_port_state(size_t port_no, volatile hba_reg_t *regs, const char *stage) {
    uint32_t ssts = regs[HBA_RPxSSTS];
    write_serial_fmt(
        "AHCI[%d] %s: CMD=0x%x TFD=0x%x SSTS=0x%x(det=%d spd=%d ipm=%d) SIG=0x%x SERR=0x%x CI=0x%x SACT=0x%x\n",
        port_no, stage, regs[HBA_RPxCMD], regs[HBA_RPxTFD], ssts, HBA_PXSSTS_DET(ssts), HBA_PXSSTS_SPD(ssts),
        HBA_PXSSTS_IPM(ssts), regs[HBA_RPxSIG], regs[HBA_RPxSERR], regs[HBA_RPxCI], regs[HBA_RPxSACT]);
}

static void ahci_enable_pci_device(pci_device_t *device) {
    uint32_t cmd_before = pci_read_command(device, PCI_CONF_COMMAND);
    uint32_t cmd_after  = cmd_before | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    if (cmd_after != cmd_before) {
        pci_write_command(device, PCI_CONF_COMMAND, cmd_after);
    }
    write_serial_fmt("AHCI: PCI command before=0x%x after=0x%x\n", cmd_before,
                     pci_read_command(device, PCI_CONF_COMMAND));
}

static void ahci_bios_handoff(struct ahci_hba *hba) {
    uint32_t cap2 = hba->base[HBA_RCAP2];
    uint32_t bohc = hba->base[HBA_RBOHC];

    write_serial_fmt("AHCI: CAP2=0x%x BOHC(before)=0x%x\n", cap2, bohc);
    if ((cap2 & HBA_CAP2_BOH) == 0) {
        write_serial_fmt("AHCI: BIOS/OS handoff not supported\n");
        return;
    }

    hba->base[HBA_RBOHC] = bohc | HBA_BOHC_OOS | HBA_BOHC_OOC;
    uint32_t timeout_ms = 2000;
    while ((hba->base[HBA_RBOHC] & HBA_BOHC_BOS) != 0 && timeout_ms-- > 0) {
        delay_ms_hp(1);
    }

    if ((hba->base[HBA_RBOHC] & HBA_BOHC_BOS) != 0) {
        write_serial_fmt("AHCI: BIOS handoff timed out, BOHC=0x%x\n", hba->base[HBA_RBOHC]);
    } else {
        write_serial_fmt("AHCI: BIOS handoff complete, BOHC=0x%x\n", hba->base[HBA_RBOHC]);
    }
}

void ahci_set_environment(bool is_qemu) {
    ahci_running_on_qemu = is_qemu;
}

bool ahci_is_qemu_environment() {
    return ahci_running_on_qemu;
}

void ahci_set_accel(bool enabled) {
    ahci_accel_enabled = enabled;
    write_serial_fmt("AHCI: acceleration %s\n", enabled ? "enabled" : "disabled");
}

bool ahci_get_accel() {
    return ahci_accel_enabled;
}

void achi_register_ops(struct hba_port *port) {
    if (!(port->device->flags & HBA_DEV_FATAPI)) {
        port->device->ops.submit = sata_submit;
    } else {
        port->device->ops.submit = scsi_submit;
    }
}

void ahci_parsestr(char *str, uint16_t *reg_start, int size_word) {
    int j = 0;
    for (int i = 0; i < size_word; i++, j += 2) {
        uint16_t reg = *(reg_start + i);
        str[j]       = (char)(reg >> 8);
        str[j + 1]   = (char)(reg & 0xff);
    }
    str[j - 1] = '\0';
}

void ahci_parse_dev_info(struct hba_device *dev_info, uint16_t *data) {
    dev_info->max_lba          = *((uint32_t *)(data + IDDEV_OFFMAXLBA));
    dev_info->block_size       = *((uint32_t *)(data + IDDEV_OFFLSECSIZE));
    dev_info->cbd_size         = cdb_size[(*data & 0x3)];
    dev_info->wwn              = *(uint64_t *)(data + IDDEV_OFFWWN);
    dev_info->block_per_sec    = 1 << (*(data + IDDEV_OFFLPP) & 0xf);
    dev_info->alignment_offset = *(data + IDDEV_OFFALIGN) & 0x3fff;
    dev_info->capabilities     = *((uint32_t *)(data + IDDEV_OFFCAPABILITIES));

    if (!dev_info->block_size) { dev_info->block_size = 512; }

    if ((*(data + IDDEV_OFFADDSUPPORT) & 0x8) && (*(data + IDDEV_OFFA48SUPPORT) & 0x400)) {
        dev_info->max_lba  = *((uint64_t *)(data + IDDEV_OFFMAXLBA_EXT));
        dev_info->flags   |= HBA_DEV_FEXTLBA;
    }

    ahci_parsestr(dev_info->serial_num, data + IDDEV_OFFSERIALNUM, 10);
    ahci_parsestr(dev_info->model, data + IDDEV_OFFMODELNUM, 20);
}

int __get_free_slot(struct hba_port *port) {
    hba_reg_t pxsact   = port->regs[HBA_RPxSACT];
    hba_reg_t pxci     = port->regs[HBA_RPxCI];
    hba_reg_t free_bmp = pxsact | pxci;
    for (uint32_t i = 0; i < port->hba->cmd_slots; i++, free_bmp >>= 1) {
        if ((free_bmp & 0x1) == 0) return (int)i;
    }
    return -1;
}

int hba_prepare_cmd(struct hba_port *port, struct hba_cmdt **cmdt, struct hba_cmdh **cmdh) {
    int slot = __get_free_slot(port);
    if (slot < 0) return -1;
    struct hba_cmdh *cmd_header = (struct hba_cmdh*)phys_to_virt((uint64_t)&port->cmdlst[slot]);
    memset(cmd_header, 0, sizeof(struct hba_cmdh));

    if (port->cmd_tables[slot] == NULL) {
        uint64_t phys = alloc_frames(1);
        if (phys == 0) return -1;
        struct hba_cmdt *cmd_table = (struct hba_cmdt *)driver_phys_to_virt(phys);
        page_map_to(get_current_directory(), (uint64_t)cmd_table, phys, KERNEL_PTE_FLAGS);
        memset(cmd_table, 0, PAGE_SIZE);
        port->cmd_tables[slot]    = cmd_table;
        port->cmd_table_phys[slot] = phys;
    }

    struct hba_cmdt *cmd_table = port->cmd_tables[slot];
    uint64_t         phys      = port->cmd_table_phys[slot];
    memset(cmd_table, 0, PAGE_SIZE);
    cmd_header->cmd_table_base       = (uint32_t)(phys & 0xFFFFFFFF);
    cmd_header->cmd_table_base_upper = (uint32_t)(phys >> 32);
    cmd_header->options = HBA_CMDH_FIS_LEN(sizeof(struct sata_reg_fis)) | HBA_CMDH_CLR_BUSY;
    *cmdh = cmd_header;
    *cmdt = cmd_table;
    return slot;
}

void __hba_reset_port(hba_reg_t *port_reg) {
    port_reg[HBA_RPxCMD] &= ~HBA_PxCMD_ST;
    port_reg[HBA_RPxCMD] &= ~HBA_PxCMD_FRE;
    uint64_t cnt = wait_until_expire((port_reg[HBA_RPxCMD] & (HBA_PxCMD_CR | HBA_PxCMD_FR)) == 0, 1000000);
    if (cnt == 0) {
        write_serial_fmt("AHCI: Port reset timeout waiting for FR/CR to clear CMD=0x%x\n", port_reg[HBA_RPxCMD]);
        return;
    }
    hba_clear_reg(port_reg[HBA_RPxIS]);
    hba_clear_reg(port_reg[HBA_RPxSERR]);
    port_reg[HBA_RPxSCTL] = (port_reg[HBA_RPxSCTL] & ~0xf) | 1;
    delay_ms_hp(1);
    port_reg[HBA_RPxSCTL] &= ~0xf;
    delay_ms_hp(10);
}

int hba_bind_vbuf(struct hba_cmdh *cmdh, struct hba_cmdt *cmdt, struct vecbuf *vbuf) {
    size_t         i   = 0;
    struct vecbuf *pos = vbuf;

    do {
        cmdt->entries[i++] = (struct hba_prdte){
            .data_base  = (uint64_t)driver_virt_to_phys((uint64_t)pos->buf.buffer),
            .byte_count = pos->buf.size - 1};
        pos = list_entry(pos->components.next, struct vecbuf, components);
    } while (pos != vbuf);

    cmdh->prdt_len = i + 1;

    return 0;
}

int hba_bind_sbuf(struct hba_cmdh *cmdh, struct hba_cmdt *cmdt, void *buf, uint32_t len) {
    if (len > 0x400000UL) {
        write_serial_fmt("AHCI buffer too large\n");
        return -1;
    }

    uint64_t buf_phys_drv  = (uint64_t)driver_virt_to_phys((uint64_t)buf);
    uint64_t cmdt_phys_drv = (uint64_t)driver_virt_to_phys((uint64_t)cmdt);
    uint64_t buf_phys      = page_virt_to_phys((uint64_t)buf);
    uint64_t cmdt_phys     = page_virt_to_phys((uint64_t)cmdt);
    if (buf_phys == 0) {
        write_serial_fmt("AHCI buffer not mapped buf=0x%llx buf_drv=0x%llx cmdt=0x%llx cmdt_drv=0x%llx\n",
                         (uint64_t)buf, buf_phys_drv, (uint64_t)cmdt, cmdt_phys_drv);
        return -1;
    }

    if (buf_phys != buf_phys_drv || cmdt_phys != cmdt_phys_drv) {
        // write_serial_fmt("AHCI TRACE: phys mismatch buf=0x%llx drv=0x%llx walk=0x%llx cmdt=0x%llx drv=0x%llx walk=0x%llx\n",
        //                  (uint64_t)buf, buf_phys_drv, buf_phys, (uint64_t)cmdt, cmdt_phys_drv, cmdt_phys);
    }

    if (buf_phys == cmdt_phys) {
        write_serial_fmt("AHCI: buffer/cmd_table physical collision buf=0x%llx phys=0x%llx len=0x%x\n",
                         (uint64_t)buf, buf_phys, len);
    }

    cmdh->prdt_len                   = 1;
    cmdt->entries[0].data_base       = buf_phys;
    cmdt->entries[0].data_base_upper = (buf_phys >> 32);
    cmdt->entries[0].byte_count      = len - 1;

    return 0;
}

void sata_create_fis(struct sata_reg_fis *cmd_fis, uint8_t command, uint64_t lba,
                     uint16_t sector_count) {
    memset(cmd_fis, 0, sizeof(struct sata_reg_fis));

    cmd_fis->head.type       = SATA_REG_FIS_H2D;
    cmd_fis->head.options    = SATA_REG_FIS_COMMAND;
    cmd_fis->head.status_cmd = command;
    cmd_fis->dev             = 0;

    cmd_fis->lba0  = SATA_LBA_COMPONENT(lba, 0);
    cmd_fis->lba8  = SATA_LBA_COMPONENT(lba, 8);
    cmd_fis->lba16 = SATA_LBA_COMPONENT(lba, 16);
    cmd_fis->lba24 = SATA_LBA_COMPONENT(lba, 24);

    cmd_fis->lba32 = SATA_LBA_COMPONENT(lba, 32);
    cmd_fis->lba40 = SATA_LBA_COMPONENT(lba, 40);

    cmd_fis->count = sector_count;
}

int ahci_init_device(struct hba_port *port) {
    struct hba_cmdt *cmd_table;
    struct hba_cmdh *cmd_header;

    // 分配并映射IDENTIFY数据缓冲�?
    uint16_t *data_in = (uint16_t *)alloc_frames(1);
    if (!data_in) {
        write_serial_fmt("AHCI: Failed to allocate buffer for IDENTIFY\n");
        return 0;
    }
    uint16_t *data = (uint16_t*)driver_phys_to_virt((uint64_t)data_in);
    page_map_to(get_current_directory(), (uint64_t)data, (uint64_t)data_in, KERNEL_PTE_FLAGS);
    memset(data, 0, 512); // 清零缓冲�?

    // 准备命令结构
    int slot = hba_prepare_cmd(port, &cmd_table, &cmd_header);
    if (slot < 0) {
        write_serial_fmt("AHCI: No free command slot\n");
        free_frame((uint64_t)data_in);
        return 0;
    }
    
    if (hba_bind_sbuf(cmd_header, cmd_table, data, 512) != 0) {
        write_serial_fmt("AHCI: Failed to bind buffer\n");
        free_frame((uint64_t)data_in);
        return 0;
    }

    // 分配并初始化设备结构
    struct sata_reg_fis *cmd_fis = NULL;
    uint32_t sig=0;
    port->device = (struct hba_device*)malloc(sizeof(struct hba_device));
    if (!port->device) {
        write_serial_fmt("AHCI: Out of memory for device struct\n");
        goto fail_cleanup;
    }
    memset(port->device, 0, sizeof(struct hba_device));
    port->device->port = port;
    port->device->hba = port->hba;

    cmd_fis = (struct sata_reg_fis *)(&cmd_table->command_fis);

    // 根据设备签名选择IDENTIFY命令
    sig = port->regs[HBA_RPxSIG];
    write_serial_fmt("AHCI: port signature=0x%x\n", sig);
    if (sig == HBA_DEV_SIG_ATA) {
        // ATA 设备
        sata_create_fis(cmd_fis, ATA_IDENTIFY_DEVICE, 0, 0);
        write_serial_fmt("AHCI: ATA device detected, sending IDENTIFY\n");
    } else if (sig == HBA_DEV_SIG_ATAPI) {
        // ATAPI 设备
        port->device->flags |= HBA_DEV_FATAPI;
        sata_create_fis(cmd_fis, ATA_IDENTIFY_PAKCET_DEVICE, 0, 0);
        write_serial_fmt("AHCI: ATAPI device detected, sending IDENTIFY PACKET\n");
    } else {
        write_serial_fmt("AHCI: Unknown device signature: 0x%x\n", sig);
        goto fail_cleanup;
    }

    // 发送IDENTIFY命令
    if (!ahci_try_send(port, slot)) {
        write_serial_fmt("AHCI: IDENTIFY command failed\n");
        goto fail_cleanup;
    }

    // 解析IDENTIFY数据
    ahci_parse_dev_info(port->device, data);

    // 如果是ATAPI设备，需要获取容量信�?
    if (port->device->flags & HBA_DEV_FATAPI) {
        write_serial_fmt("AHCI: Initializing ATAPI device\n");
        
        // 准备READ CAPACITY命令
        memset(cmd_table, 0, sizeof(struct hba_cmdt)); // 清空命令�?
        sata_create_fis(cmd_fis, ATA_PACKET, 0, 0); // 设置PACKET FIS
        
        // 根据CDB大小创建SCSI命令
        if (port->device->cbd_size == SCSI_CDB12) {
            struct scsi_cdb12 *cdb12 = (struct scsi_cdb12 *)cmd_table->atapi_cmd;
            scsi_create_packet12(cdb12, SCSI_READ_CAPACITY_10, 0, 0);
        } else {
            struct scsi_cdb16 *cdb16 = (struct scsi_cdb16 *)cmd_table->atapi_cmd;
            scsi_create_packet16(cdb16, SCSI_READ_CAPACITY_16, 0, 0);
            cdb16->misc1 = 0x10; // service action for READ CAPACITY(16)
        }

        // 重新绑定数据缓冲�?
        memset(data, 0, 512);
        if (hba_bind_sbuf(cmd_header, cmd_table, data, 512) != 0) {
            write_serial_fmt("AHCI: Failed to bind buffer for READ CAPACITY\n");
            goto fail_cleanup;
        }

        cmd_header->options |= HBA_CMDH_ATAPI;
        cmd_header->transferred_size = 0;

        // 发送READ CAPACITY命令
        if (!ahci_try_send(port, slot)) {
            write_serial_fmt("AHCI: READ CAPACITY command failed\n");
            goto fail_cleanup;
        }

        // 解析容量信息
        scsi_parse_capacity(port->device, (uint32_t *)data);
    }

    // 注册设备操作
    achi_register_ops(port);

    write_serial_fmt("AHCI: Device initialized: %s, blocks: %d, block_size: %d\n",
          port->device->model, port->device->max_lba, port->device->block_size);

    // 清理资源
    free_frame((uint64_t)data_in);
    return 1;

fail_cleanup:
    if (port->device) {
        free(port->device);
        port->device = NULL;
    }
    free_frame((uint64_t)data_in);
    return 0;
}

spin_t ahci_operate_lock = SPIN_INIT;

size_t ahci_read(int drive, uint8_t *buffer, size_t size, size_t lba) {
    spin_lock(&ahci_operate_lock);
    struct hba_device *dev = (struct hba_device *)hbaDevice[drive];
    if (dev == NULL) {
        write_serial_fmt("AHCI: read on missing device drive=%d lba=%llu sectors=%llu\n", drive, lba, size);
        spin_unlock(&ahci_operate_lock);
        return 0;
    }
    struct blkio_req   req = {
          .buf = (uint64_t)buffer, .lba = lba, .len = size * dev->block_size, .flags = 0, .status = -1};
    dev->ops.submit(dev, &req);
    spin_unlock(&ahci_operate_lock);
    if (req.status != 0) {
        write_serial_fmt("AHCI: read failed drive=%d lba=%llu sectors=%llu bytes=%llu\n", drive, lba, size, req.len);
        return 0;
    }
    return size;
}

size_t ahci_write(int drive, uint8_t *buffer, size_t size, size_t lba) {
    spin_lock(&ahci_operate_lock);
    struct hba_device *dev = (struct hba_device *)hbaDevice[drive];
    if (dev == NULL) {
        write_serial_fmt("AHCI: write on missing device drive=%d lba=%llu sectors=%llu\n", drive, lba, size);
        spin_unlock(&ahci_operate_lock);
        return 0;
    }
    struct blkio_req   req = {
          .buf = (uint64_t)buffer, .lba = lba, .len = size * dev->block_size, .flags = BLKIO_WRITE, .status = -1};
    dev->ops.submit(dev, &req);
    spin_unlock(&ahci_operate_lock);
    if (req.status != 0) {
        write_serial_fmt("AHCI: write failed drive=%d lba=%llu sectors=%llu bytes=%llu\n", drive, lba, size, req.len);
        return 0;
    }
    return size;
}

static int ahci_ioctl(device_t *device, size_t req, void *arg)
{
    (void)device;
    (void)arg;
    return req == IOBLKSYNC ? 0 : -1;
}

// �?utils.cpp 中添�?
void ahci_setup() {
    pci_device_t *device = pci_find_class(0x010601);
    if (device == NULL) {
        write_serial_fmt("AHCI: No AHCI controller found\n");
        return;
    }
    write_serial_fmt("AHCI: environment=%s controller=%02x:%02x.%x vendor=0x%x device=0x%x\n",
                     ahci_running_on_qemu ? "qemu-like" : "generic/vmware/real", device->bus, device->slot,
                     device->func, device->vendor_id, device->device_id);
    ahci_enable_pci_device(device);
    pci_bar_base_address bar5 = device->bars[5];
    if (bar5.address == 0 || bar5.size == 0) {
        write_serial_fmt("AHCI: BAR5 is not valid, skipping AHCI setup\n");
        return;
    }
    op_buffer = (void *)alloc_frames(0x400000UL / PAGE_SIZE);
    struct ahci_hba *hba = (struct ahci_hba*)malloc(sizeof(struct ahci_hba));
    memset(hba, 0, sizeof(struct ahci_hba));
    hba->base = (hba_reg_t *)phys_to_virt(bar5.address);
    hba->version = hba->base[HBA_RVER];
    uint32_t major = (hba->version >> 16) & 0xFF;
    uint32_t minor = (hba->version >> 8) & 0xFF;
    uint32_t patch = hba->version & 0xFF;
    uint32_t cap   = hba->base[HBA_RCAP];
    uint32_t cap2  = hba->base[HBA_RCAP2];
    uint32_t pmap  = hba->base[HBA_RPI];
    write_serial_fmt("AHCI: BAR5=0x%x size=0x%x CAP=0x%x CAP2=0x%x PI=0x%x VS=%d.%d.%d\n",
                     (uint32_t)bar5.address, (uint32_t)bar5.size, cap, cap2, pmap, major, minor, patch);
    ahci_bios_handoff(hba);
    hba->base[HBA_RGHC] |= HBA_RGHC_ACHI_ENABLE;
    uint32_t timeout = 1000000;
    while ((hba->base[HBA_RGHC] & HBA_RGHC_ACHI_ENABLE) == 0 && timeout-- > 0) {
        asm volatile("pause");
    }
    if (timeout == 0) {
        write_serial_fmt("AHCI: Failed to enable AHCI mode\n");
        free(hba);
        return;
    }
    cap            = hba->base[HBA_RCAP];
    cap2           = hba->base[HBA_RCAP2];
    pmap           = hba->base[HBA_RPI];
    hba->ports_num = (cap & HBA_CAP_NP) + 1;
    hba->cmd_slots = ((cap & HBA_CAP_NCS) >> 8) + 1;
    hba->ports_bmp = pmap;
    write_serial_fmt("AHCI: mode enabled CAP=0x%x CAP2=0x%x ports=%d cmd_slots=%d PI=0x%x\n", cap, cap2,
                     hba->ports_num, hba->cmd_slots, pmap);
    uint32_t online_ports = 0;
    uint32_t ready_ports  = 0;
    for (size_t i = 0; i < hba->ports_num && i < 32; i++) {
        if ((pmap & (1u << i)) == 0) continue;
        struct hba_port *port = (struct hba_port *)malloc(sizeof(struct hba_port));
        memset(port, 0, sizeof(struct hba_port));
        hba_reg_t *port_regs = (hba_reg_t *)(&hba->base[HBA_RPBASE + i * HBA_RPSIZE]);
        port->regs = port_regs;
        port->hba  = hba;
        hba->ports[i] = port;
        // ahci_log_port_state(i, port_regs, "before-reset");
        __hba_reset_port((hba_reg_t *)port_regs);
        uint64_t clb_pa = alloc_frames(1);
        uint64_t fis_pa = alloc_frames(1);
        if (clb_pa == 0 || fis_pa == 0) {
            write_serial_fmt("AHCI[%d] failed to allocate CLB/FIS buffers clb=0x%x fis=0x%x\n", i,
                             (uint32_t)clb_pa, (uint32_t)fis_pa);
            continue;
        }
        memset((void *)phys_to_virt(clb_pa), 0, PAGE_SIZE);
        memset((void *)phys_to_virt(fis_pa), 0, PAGE_SIZE);
        port_regs[HBA_RPxCLB]     = (uint32_t)(clb_pa & 0xFFFFFFFF);
        port_regs[HBA_RPxCLB + 1] = (uint32_t)(clb_pa >> 32);
        port_regs[HBA_RPxFB]      = (uint32_t)(fis_pa & 0xFFFFFFFF);
        port_regs[HBA_RPxFB + 1]  = (uint32_t)(fis_pa >> 32);
        port->cmdlst = (struct hba_cmdh *)(uintptr_t)clb_pa;
        port->fis    = (void *)(uintptr_t)fis_pa;
        port_regs[HBA_RPxCI] = 0;
        port_regs[HBA_RPxSACT] = 0;
        hba_clear_reg(port_regs[HBA_RPxIS]);
        hba_clear_reg(port_regs[HBA_RPxSERR]);
        port_regs[HBA_RPxCMD] |= HBA_PxCMD_FRE;
        port_regs[HBA_RPxCMD] |= HBA_PxCMD_ST;
        delay_ms_hp(1);
        port->ssts = port_regs[HBA_RPxSSTS];
        // ahci_log_port_state(i, port_regs, "after-start");
        if (!ahci_port_link_ready(port->ssts)) {
            // write_serial_fmt("AHCI[%d] skip: link not ready det=%d ipm=%d spd=%d\n", i,
                            //  HBA_PXSSTS_DET(port->ssts), HBA_PXSSTS_IPM(port->ssts), HBA_PXSSTS_SPD(port->ssts));
            continue;
        }
        online_ports++;
        if (!ahci_init_device(port)) {
            write_serial_fmt("AHCI[%d] device init failed after link-up\n", i);
            ahci_log_port_state(i, port_regs, "init-failed");
            continue;
        }
        ready_ports++;
        struct hba_device *hbadev = port->device;
        char name_buf[20];
        sprintf(name_buf, "sata%d", i);
        device_t sata;
        sata.size = hbadev->max_lba * hbadev->block_size;
        sata.sector_size = hbadev->block_size;
        sata.type = DEVICE_BLOCK;
        sata.read = ahci_read;
        sata.write = (hbadev->flags & HBA_DEV_FATAPI) ? NULL : ahci_write;
        sata.flag = 1;
        sata.ioctl = ahci_ioctl;
        sata.poll = (pollf)empty;
        sata.map = (mapf)empty;
        sata.read_vbuf = NULL;
        sata.write_vbuf = NULL;
        sata.max_size = __UINT64_MAX__;
        strcpy(sata.drive_name, name_buf);
        int id = regist_device(NULL, sata);
        hbaDevice[id] = hbadev;
        write_serial_fmt("sata%d: blk_size=%d, blk=0..%d, %s%s\n", i, hbadev->block_size, hbadev->max_lba,
                         hbadev->model, (hbadev->flags & HBA_DEV_FATAPI) ? " [ATAPI read-only]" : "");
    }
    write_serial_fmt("AHCI initialized: online_ports=%d ready_ports=%d implemented_ports=%d version=%d.%d.%d\n",
                     online_ports, ready_ports, hba->ports_num, major, minor, patch);
}


