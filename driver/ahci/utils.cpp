#include "ahci/ahci.h"
#include "proto.hpp"

extern bool ahci_get_accel();

static void ahci_log_cmd_failure(struct hba_port *port, int slot, const char *reason) {
    write_serial_fmt(
        "AHCI: %s slot=%d CI=0x%x SACT=0x%x IS=0x%x TFD=0x%x SERR=0x%x SSTS=0x%x CMD=0x%x\n", reason, slot,
        port->regs[HBA_RPxCI], port->regs[HBA_RPxSACT], port->regs[HBA_RPxIS], port->regs[HBA_RPxTFD],
        port->regs[HBA_RPxSERR], port->regs[HBA_RPxSSTS], port->regs[HBA_RPxCMD]);
}

static int ahci_try_send_safe(struct hba_port *port, int slot)
{
    uint32_t bitmask = 1u << slot;
    uint64_t ready   = wait_until_expire(!(port->regs[HBA_RPxTFD] & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ)), 1000000);
    if (ready == 0) {
        ahci_log_cmd_failure(port, slot, "device busy timeout");
        return 0;
    }

    hba_clear_reg(port->regs[HBA_RPxIS]);
    hba_clear_reg(port->regs[HBA_RPxSERR]);

    for (int retries = 0; retries < 3; retries++) {
        port->regs[HBA_RPxCI] |= bitmask;

        uint64_t complete = 10000000;
        while ((port->regs[HBA_RPxCI] & bitmask) != 0 && complete-- > 0) {
            if (port->regs[HBA_RPxIS] & HBA_FATAL) break;
            asm volatile("pause");
        }

        if ((port->regs[HBA_RPxCI] & bitmask) != 0) {
            port->regs[HBA_RPxCI] &= ~bitmask;
            ahci_log_cmd_failure(port, slot, "command completion timeout");
        } else if ((port->regs[HBA_RPxIS] & HBA_FATAL) || (port->regs[HBA_RPxTFD] & HBA_PxTFD_ERR)) {
            sata_read_error(port);
            ahci_log_cmd_failure(port, slot, "task file error");
        } else {
            uint64_t settle = 100000;
            while (settle-- > 0) {
                uint32_t is = port->regs[HBA_RPxIS];
                if (is & (HBA_PxINTR_DPS | HBA_PxINTR_DHR | HBA_FATAL)) break;
                asm volatile("pause");
            }
            io_mfence();
            delay_us_hp(10);
            hba_clear_reg(port->regs[HBA_RPxIS]);
            return 1;
        }

        hba_clear_reg(port->regs[HBA_RPxIS]);
        hba_clear_reg(port->regs[HBA_RPxSERR]);
        delay_ms_hp(1);
    }

    return 0;
}

static int ahci_wait_port_ready(struct hba_port *port, uint64_t timeout_ns)
{
    uint64_t deadline = nanoTime() + timeout_ns;
    while (port->regs[HBA_RPxTFD] & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ)) {
        if (nanoTime() >= deadline) { return 0; }
        asm volatile("pause");
    }
    return 1;
}

int ahci_try_send(struct hba_port *port, int slot)
{
    if (slot < 0 || slot >= 32) {
        write_serial_fmt("AHCI: invalid slot %d\n", slot);
        return 0;
    }

    if (!ahci_get_accel()) {
        return ahci_try_send_safe(port, slot);
    }

    uint32_t bitmask = 1u << slot;
    if (!ahci_wait_port_ready(port, 100000000ULL)) {
        ahci_log_cmd_failure(port, slot, "device busy timeout");
        return 0;
    }

    hba_clear_reg(port->regs[HBA_RPxIS]);
    hba_clear_reg(port->regs[HBA_RPxSERR]);

    for (int retries = 0; retries < 3; retries++) {
        port->regs[HBA_RPxCI] |= bitmask;

        uint64_t deadline = nanoTime() + 2000000000ULL;
        while ((port->regs[HBA_RPxCI] & bitmask) != 0) {
            if (port->regs[HBA_RPxIS] & HBA_FATAL) break;
            if (nanoTime() >= deadline) break;
            asm volatile("pause");
        }

        if ((port->regs[HBA_RPxCI] & bitmask) != 0) {
            port->regs[HBA_RPxCI] &= ~bitmask;
            ahci_log_cmd_failure(port, slot, "command completion timeout");
        } else if ((port->regs[HBA_RPxIS] & HBA_FATAL) || (port->regs[HBA_RPxTFD] & HBA_PxTFD_ERR)) {
            sata_read_error(port);
            ahci_log_cmd_failure(port, slot, "task file error");
        } else {
            io_mfence();
            hba_clear_reg(port->regs[HBA_RPxIS]);
            return 1;
        }

        hba_clear_reg(port->regs[HBA_RPxIS]);
        hba_clear_reg(port->regs[HBA_RPxSERR]);
        delay_ms_hp(1);
    }

    return 0;
}

void ahci_post(struct hba_port *port, struct hba_cmd_state *state, int slot)
{
    uint32_t bitmask = 1u << slot;
    port->cmdctx.issued[slot] = state;
    port->cmdctx.tracked_ci  |= bitmask;
    if (!ahci_try_send(port, slot)) {
        write_serial_fmt("AHCI: async post failed for slot=%d\n", slot);
    }
    port->cmdctx.tracked_ci &= ~bitmask;
}
