# Native IDE storage

XJ380 includes a first-party PCI IDE controller driver for the legacy task-file
interface. It uses bounded polling PIO and does not enable DMA, bus mastering,
or controller mode changes.

The driver accepts PCI mass-storage class `01:01` controllers with a normal
header and an enabled I/O command bit. Each channel is decoded independently:
firmware-provided native BARs are used when the programming interface selects
native mode; otherwise the standard primary (`1f0/3f6`) and secondary
(`170/376`) compatibility ports are used. Invalid BARs and overlapping channel
ranges are rejected before probing.

ATA disks must advertise LBA. LBA28 and LBA48 IDENTIFY data are validated,
including the logical-sector description. The transport always transfers 512
byte logical sectors and accepts 512e devices; 4Kn logical devices are refused.
LBA48 commands are selected for addresses at or above `2^28` and for requests
that cross that boundary. Requests are checked completely before any task-file
register is written.

ATA reads, writes, and cache flushes are supported. A disk is registered as a
durable block source only when it advertises the matching cache-flush command.
ATAPI devices are read-only and use bounded PACKET/READ CAPACITY transport;
media-change Unit Attention is cleared once with REQUEST SENSE. Unsupported
media, malformed identify data, and incomplete phases are skipped or rejected.

Every channel has one serialized operation. Runtime operations pin their task
through `KernelOperationScope` before taking the channel mutex. A task-file
timeout, status error, or uncertain completion quarantines the whole channel
until reboot and rediscovery; the driver does not replay an uncertain write or
steal an interrupted channel for recovery. The fatal write-on-system-down path
uses an atomic channel claim, supports at most eight 512-byte sectors per
call, and requires an ATA disk with a verified flush command.

The implementation is intentionally PIO-only and polling-only. DMA, interrupts,
controller reset/reprobe, removable-media writes, and 4Kn logical sectors are
outside this first release.

Host coverage is in `tests/test_ide.py` and `tests/test_ide_atapi.py`; run both
with `python3 -m unittest discover -s tests -p 'test_ide*.py' -v`. The production
source inventory can be checked with `python3 OpenXJ380/tools/check_ide_sources.py`.

The real-controller acceptance test is `python3 tests/run_ide_qemu.py`. It boots
the staged OS using QEMU's PIIX3 PCI IDE controller, attaches a 140 GiB sparse
slave disk to the boot disk's channel, writes and reads three sectors above the
LBA28 limit, issues the registered block device's sync ioctl, reaches normal
login, and verifies the flushed bytes from the host after QEMU exits.
