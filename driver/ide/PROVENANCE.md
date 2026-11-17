# IDE driver provenance

`ide.cpp` is original first-party C++ written for the freestanding OpenXJ380
kernel and licensed under Apache-2.0. It uses the existing PCI, port-I/O, VFS,
block-source, mutex, scheduler, termination, and WSOD interfaces. No external
IDE implementation, generated port, or link archive is included.

The implementation was developed from public ATA task-file and IDENTIFY
semantics, PCI IDE programming-interface rules, and independently scripted
PIO fixtures. The production driver is tested as a complete translation unit
against the scripted boundary in `tests/ide_ata_harness.cpp` and
`tests/ide_atapi_harness.cpp`; those host fixtures are first-party test code.

The source inventory in `SOURCE.json` records the reviewed implementation hash.
Update it only after reviewing the changed source and its provenance.
