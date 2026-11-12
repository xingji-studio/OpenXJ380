// SPDX-License-Identifier: Apache-2.0
// Original bounded physical-source provenance for installer safety.
#include <block_source.h>
#include <errno.h>
#include <pci/mcfg.h>

extern pci_device_op_t pci_device_op, pcie_device_op;
extern MCFG_ENTRY *mcfg_entries[PCI_MCFG_MAX_ENTRIES_LEN];
extern uint64_t mcfg_entries_len;

namespace {

constexpr unsigned kSourceDevices = 256;
struct SourceRegistration
{
    BlockSourceHardware hardware;
    uint64_t generation;
    bool valid;
};
SourceRegistration source_devices[kSourceDevices];
unsigned char source_lock;
uint64_t source_revision;

static uint64_t source_irq_save()
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void source_irq_restore(uint64_t flags)
{
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

class SourceLock
{
    uint64_t flags;
public:
    SourceLock() : flags(source_irq_save())
    {
        while (__atomic_test_and_set(&source_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    }
    ~SourceLock()
    {
        __atomic_clear(&source_lock, __ATOMIC_RELEASE);
        source_irq_restore(flags);
    }
};

// Existing ECAM access chooses the first entry for a segment. Refuse split
// or unknown layouts rather than probing an out-of-range MMIO bus while
// authenticating firmware-supplied parent paths. Legacy config is segment 0.
static bool config_available(const pci_device_t *controller, uint16_t segment, uint8_t bus)
{
    if (controller->op == &pci_device_op) return segment == 0;
    if (controller->op != &pcie_device_op || mcfg_entries_len > PCI_MCFG_MAX_ENTRIES_LEN) return false;
    const MCFG_ENTRY *entry = nullptr;
    for (uint64_t i = 0; i < mcfg_entries_len; ++i)
    {
        if (!mcfg_entries[i] || mcfg_entries[i]->pci_segment_group != segment) continue;
        if (entry) return false;
        entry = mcfg_entries[i];
    }
    return entry && entry->start_bus <= bus && bus <= entry->end_bus;
}

static bool valid_hardware(const BlockSourceHardware &h)
{
    if (!h.controller || !h.controller->op || !h.controller->op->read || h.controller->slot > 31 ||
        h.controller->func > 7 || h.block_size < 512 || h.block_size > 65536 ||
        (h.block_size & (h.block_size - 1)) || !h.bytes || h.bytes % h.block_size ||
        !config_available(h.controller, h.controller->segment, h.controller->bus)) return false;
    if (h.transport == XJ_INSTALLER_SOURCE_AHCI)
        return h.controller->class_code == 0x010601 && h.sata_port < 32 && h.sata_multiplier == 0xffff &&
               !h.lun && !h.ide_channel && !h.ide_unit && !h.nvme_namespace &&
               xj_installer_bytes_zero(h.nvme_eui64, sizeof(h.nvme_eui64));
    if (h.transport == XJ_INSTALLER_SOURCE_NVME)
        return (h.controller->class_code >> 8) == 0x0108 && h.nvme_namespace &&
               h.nvme_namespace != 0xffffffffU && !h.sata_port && !h.sata_multiplier && !h.lun &&
               !h.ide_channel && !h.ide_unit;
    if (h.transport == XJ_INSTALLER_SOURCE_IDE)
        return (h.controller->class_code >> 8) == 0x0101 && h.ide_channel <= 1 && h.ide_unit <= 1 &&
               !h.sata_port && !h.sata_multiplier && !h.lun && !h.nvme_namespace &&
               xj_installer_bytes_zero(h.nvme_eui64, sizeof(h.nvme_eui64));
    return false;
}

static bool same_hardware(const BlockSourceHardware &h, const XJ_INSTALLER_SOURCE &s)
{
    const XJ_INSTALLER_PCI_LOCATION &leaf = s.pci[s.pci_count - 1];
    return h.controller && h.controller->segment == leaf.segment && h.controller->bus == leaf.bus &&
           h.controller->slot == leaf.device && h.controller->func == leaf.function &&
           h.transport == s.transport && h.sata_port == s.sata_port &&
           h.sata_multiplier == s.sata_multiplier && h.lun == s.lun && h.ide_channel == s.ide_channel &&
           h.ide_unit == s.ide_unit && h.nvme_namespace == s.nvme_namespace;
}

static bool authentic_pci_path(const BlockSourceHardware &h, const XJ_INSTALLER_SOURCE &s)
{
    // Firmware supplies each actual BDF via PCI_IO.GetLocation. Authenticate
    // every bridge edge using the independently initialized kernel controller's
    // PCI config accessor; pci_devices intentionally retains endpoints only.
    for (unsigned i = 0; i + 1 < s.pci_count; ++i)
    {
        const auto &parent = s.pci[i];
        const auto &child = s.pci[i + 1];
        if (!config_available(h.controller, parent.segment, parent.bus)) return false;
        auto read = h.controller->op->read;
        uint32_t id = read(parent.bus, parent.device, parent.function, parent.segment, 0);
        uint32_t class_revision = read(parent.bus, parent.device, parent.function, parent.segment, 8);
        uint32_t header = read(parent.bus, parent.device, parent.function, parent.segment, 12);
        uint32_t buses = read(parent.bus, parent.device, parent.function, parent.segment, 24);
        if ((id & 0xffff) == 0xffff || (class_revision >> 16) != 0x0604 ||
            ((header >> 16) & 0x7f) != 1 || (buses & 0xff) != parent.bus ||
            ((buses >> 8) & 0xff) != child.bus || ((buses >> 16) & 0xff) < child.bus) return false;
    }
    return true;
}

} // namespace

int block_source_register(uint32_t device, const BlockSourceHardware &hardware)
{
    if (device >= kSourceDevices || !valid_hardware(hardware)) return -EINVAL;
    BlockExtent whole;
    int result = block_access_resolve(device, 0, &whole);
    if (result) return result;
    if (whole.device != device || whole.physical_device != device || whole.start ||
        whole.generation != whole.physical_generation || whole.bytes != hardware.bytes) return -EINVAL;
    SourceLock lock;
    if (source_revision == uint64_t(-1)) return -EOVERFLOW;
    SourceRegistration &entry = source_devices[device];
    if (entry.valid && entry.generation == whole.generation) return -EEXIST;
    entry.hardware = hardware;
    entry.generation = whole.generation;
    entry.valid = true;
    ++source_revision;
    return 0;
}

int block_source_match(const XJ_INSTALLER_SOURCE &source, BlockExtent *whole)
{
    if (!whole) return -EINVAL;
    *whole = {};
    if (!xj_installer_source_valid(&source)) return -EOPNOTSUPP;
    uint64_t revision;
    {
        SourceLock lock;
        revision = source_revision;
    }
    unsigned matches = 0;
    BlockExtent candidate = {};
    for (unsigned device = 0; device < kSourceDevices; ++device)
    {
        SourceRegistration entry;
        {
            SourceLock lock;
            entry = source_devices[device];
        }
        if (!entry.valid || !same_hardware(entry.hardware, source)) continue;
        // Do not silently discard stale/ambiguous hardware identities and pick
        // another slot. Re-registration requires a fresh trusted declaration.
        BlockExtent extent;
        int result = block_access_resolve(device, entry.generation, &extent);
        if (result || extent.device != extent.physical_device || extent.start ||
            extent.generation != extent.physical_generation) return -ESTALE;
        if (++matches > 1) return -EEXIST;
        const BlockSourceHardware &h = entry.hardware;
        if (h.block_size != source.physical_block_size || h.bytes != source.physical_bytes ||
            extent.bytes != source.physical_bytes ||
            h.read_only != bool(source.flags & XJ_INSTALLER_SOURCE_READ_ONLY) || !authentic_pci_path(h, source))
            return -ESTALE;
        if (source.transport == XJ_INSTALLER_SOURCE_NVME &&
            !xj_installer_bytes_zero(source.nvme_eui64, sizeof(source.nvme_eui64)))
            for (unsigned i = 0; i < sizeof(source.nvme_eui64); ++i)
                if (source.nvme_eui64[i] != h.nvme_eui64[i]) return -ESTALE;
        candidate = extent;
    }
    {
        SourceLock lock;
        if (revision != source_revision) return -ESTALE;
    }
    if (matches != 1) return -ENODEV;
    // Authenticate again after the bounded scan. The captured generation is
    // still mandatory when the caller subsequently opens/pins the devfs node.
    BlockExtent current;
    if (block_access_resolve(candidate.device, candidate.generation, &current) ||
        current.physical_generation != candidate.physical_generation) return -ESTALE;
    *whole = current;
    return 0;
}
