// SPDX-License-Identifier: Apache-2.0
#include "ahci/ahci.h"
#include "proto.hpp"
#include <errno.h>
#include "deadline.h"

static void ahci_log_cmd_failure(struct hba_port *port, int slot, const char *reason)
{
    write_serial_fmt(
        "AHCI: %s slot=%d CI=0x%x SACT=0x%x IS=0x%x TFD=0x%x SERR=0x%x SSTS=0x%x CMD=0x%x\n", reason, slot,
        port->regs[HBA_RPxCI], port->regs[HBA_RPxSACT], port->regs[HBA_RPxIS], port->regs[HBA_RPxTFD],
        port->regs[HBA_RPxSERR], port->regs[HBA_RPxSSTS], port->regs[HBA_RPxCMD]);
}

static void ahci_log_timeout(struct hba_port *port, int slot, const char *reason,
                             const AhciDeadline &deadline, uint64_t timeout_ns)
{
    ahci_log_cmd_failure(port, slot, reason);
    uint8_t opcode = port->cmd_tables[slot] ? port->cmd_tables[slot]->command_fis[2] : 0;
    write_serial_fmt(
        "AHCI deadline: opcode=0x%x elapsed_ns=%llu budget_ns=%llu polls=%llu start_ns=%llu "
        "sample_ns=%llu clock_available=%u clock_progress=%u stagnant_polls=%llu\n",
        opcode, deadline.elapsed_ns(), timeout_ns, deadline.poll_count(), deadline.start_ns(), deadline.sample_ns(),
        deadline.clock_available() ? 1u : 0u, deadline.clock_progress() ? 1u : 0u, deadline.stagnant_polls());
}

static int ahci_command_error(struct hba_port *port)
{
    if (HBA_PXSSTS_DET(port->regs[HBA_RPxSSTS]) != 3) return -ENODEV;
    if ((port->regs[HBA_RPxIS] & (HBA_FATAL | HBA_NONFATAL)) ||
        (port->regs[HBA_RPxTFD] & (HBA_PxTFD_ERR | HBA_PxTFD_DF))) return -EIO;
    return 0;
}

int ahci_send_command(struct hba_port *port, int slot, uint64_t timeout_ns)
{
    if (!port || !port->hba || !port->regs || slot < 0 ||
        (uint32_t)slot >= port->hba->cmd_slots || slot >= 32) return -EINVAL;
    if (port->io_error) return port->io_error;
    if (port->hba->quarantined) return -EIO;
    if (!(port->regs[HBA_RPxCMD] & HBA_PxCMD_ST) || !(port->regs[HBA_RPxCMD] & HBA_PxCMD_FRE))
        return ahci_fail_port(port, -ENODEV);
    if (HBA_PXSSTS_DET(port->regs[HBA_RPxSSTS]) != 3) return ahci_fail_port(port, -ENODEV);

    // Data I/O and flush hold the same operation mutex. No command may be
    // ordered after an earlier DMA operation whose completion is unknown.
    AhciDeadline deadline(nanoTime());
    while ((port->regs[HBA_RPxTFD] & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ)) ||
           port->regs[HBA_RPxCI] || port->regs[HBA_RPxSACT])
    {
        if (HBA_PXSSTS_DET(port->regs[HBA_RPxSSTS]) != 3)
            return ahci_fail_port(port, -ENODEV);
        if (deadline.expired(nanoTime(), timeout_ns))
        {
            ahci_log_timeout(port, slot, "device ready timeout", deadline, timeout_ns);
            return ahci_fail_port(port, -ETIMEDOUT);
        }
        asm volatile("pause");
    }
    hba_clear_reg(port->regs[HBA_RPxIS]);
    hba_clear_reg(port->regs[HBA_RPxSERR]);
    if (port->regs[HBA_RPxTFD] & (HBA_PxTFD_ERR | HBA_PxTFD_DF))
        return ahci_fail_port(port, -EIO);
    io_mfence();
    uint32_t bitmask = 1u << slot;
    port->regs[HBA_RPxCI] = bitmask;
    // PxCI is RW1: software never clears an issued bit, and a failed command
    // is never blindly replayed. Recovery must establish a new port lifetime.
    deadline.restart(nanoTime());
    while (port->regs[HBA_RPxCI] & bitmask)
    {
        int error = ahci_command_error(port);
        if (error)
        {
            if (port->device) sata_read_error(port);
            ahci_log_cmd_failure(port, slot, "command task-file or transport error");
            return ahci_fail_port(port, error);
        }
        if (deadline.expired(nanoTime(), timeout_ns))
        {
            ahci_log_timeout(port, slot, "command completion timeout", deadline, timeout_ns);
            return ahci_fail_port(port, -ETIMEDOUT);
        }
        asm volatile("pause");
    }
    io_mfence();
    int error = ahci_command_error(port);
    if (!error && (port->regs[HBA_RPxTFD] & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ))) error = -EIO;
    if (error)
    {
        if (port->device) sata_read_error(port);
        ahci_log_cmd_failure(port, slot, "invalid final completion state");
        return ahci_fail_port(port, error);
    }
    hba_clear_reg(port->regs[HBA_RPxIS]);
    return 0;
}

int ahci_try_send(struct hba_port *port, int slot)
{
    return ahci_send_command(port, slot, 5000000000ULL) == 0;
}

void ahci_post(struct hba_port *port, struct hba_cmd_state *state, int slot)
{
    if (slot < 0 || slot >= 32) return;
    uint32_t bitmask = 1u << slot;
    port->cmdctx.issued[slot] = state;
    port->cmdctx.tracked_ci |= bitmask;
    if (!ahci_try_send(port, slot)) write_serial_fmt("AHCI: async post failed for slot=%d\n", slot);
    port->cmdctx.tracked_ci &= ~bitmask;
    port->cmdctx.issued[slot] = NULL;
}
