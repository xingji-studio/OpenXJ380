# AHCI flush and failed-command contract

The new flush and command-completion code is first-party Apache-2.0 code. It
uses hardware interface facts from Intel's AHCI 1.3.1 specification and the ATA
command descriptions in the Western Digital SATA product specification. No
external driver implementation is imported or translated.

- [AHCI 1.3.1](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf), sections 3.3.14 and 4.2.2.
- [Western Digital SATA interface specification](https://documents.westerndigital.com/content/dam/doc-library/en_us/assets/public/western-digital/product/data-center-drives/ultrastar-dc-hc600-series/product-manual-ultrastar-dc-hc670-sata-spec.pdf), IDENTIFY DEVICE and sections 12.9/12.10.

`IOBLKSYNC` selects the non-data FLUSH CACHE EXT (EAh) or FLUSH CACHE (E7h)
command from validated IDENTIFY support bits. PRDT length, byte count, data
address, sector count, and LBA are zero. Unsupported devices return
`EOPNOTSUPP`; ATAPI drives do not advertise writable-cache synchronization.

Runtime read, write, and flush operations use the same sleeping mutex, so a
flush follows completion of all earlier requests acquired before it. The
single-CPU discovery path before a current task exists does not use the mutex's
NULL-owner representation. Block I/O is a thread/boot-context interface, not an
interrupt-handler interface. Command tables are allocated and mapped before
device registration; request-time preparation performs no allocation or mapping.

Completion checks include transport interrupts, ATA ERR/DF, device presence,
CI completion, final BSY/DRQ, and transferred bytes for data commands. Timeouts
and ambiguous errors are latched. Software does not clear CI or replay a failed
command. Before returning the caller's DMA buffer, a failed port stops ST/CR and
FRE/FR. If the engines cannot stop, the driver attempts to remove PCI bus-master
access and marks all controller ports unavailable. It then stops the CPU without
returning the caller's buffer: bus-master readback alone does not prove that
already-issued DMA has drained. This applies even when disabling bus mastering
succeeds. Returning the buffer without confirmed quiescence would permit late
transfers into released memory.
Recovery requires complete device/controller reinitialization; this change does
not add an automatic runtime reset/retry policy.

IDENTIFY geometry now uses words 100..103 for LBA48, validates command/sector
information words, and converts logical-sector words 117/118 to bytes. Invalid
advertised geometry is rejected. Legacy 28-bit DMA requests are split within
their count field and encode the upper LBA nibble in the device field.

`tests/test_ahci_flush.py` compiles production command, flush, IDENTIFY, and
failed-port functions against a scripted MMIO model with ASan/UBSan. It covers
non-data commands, fallback E7h, unsupported capability, ERR/DF/transport/final
errors, timeouts, PCI quarantine, short transfers, and request-time storage
reuse. Real AHCI/QEMU write/flush/readback and independent filesystem checks
are additionally required for hardware integration evidence.

The polling fallback is counted only while the hardware clock does not advance;
every advancing observation resets that counter. It cannot preempt the specified
wall-clock timeout on a functioning clock. The host test substitutes only the
terminal privileged halt instruction to verify both successful and unsuccessful
PCI quarantine paths retain DMA buffer ownership.

Timeout-only diagnostics additionally report the command opcode, sampled/start
clock values, elapsed time, configured budget, poll count, and clock progress.
They do not change the timeout budget or retry failed commands. The HPET time
source now takes a stable high/low/high counter snapshot instead of trusting a
64-bit CPU load to reach the device atomically. `tests/test_hpet_counter.py`
demonstrates a split-read rollover falsely expiring the real 30-second AHCI
deadline, then checks the production snapshot under concurrent readers and
nanosecond conversion with fractional periods and long uptimes.
