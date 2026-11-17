# IDE driver license audit

Review date: 2026-11-17.

The IDE controller implementation in `driver/ide/ide.cpp` and its public
header are new first-party OpenXJ380 code under Apache-2.0. Each source file
has an SPDX identifier. No NetBSD, xv6, Linux, BSD kernel, or other upstream
IDE implementation was copied, translated, or linked into the kernel.

Public ATA task-file, IDENTIFY, LBA48, PCI IDE programming-interface, and QEMU
device behavior descriptions were used as engineering references. Reference
material is not source input and does not create an additional runtime
dependency. QEMU is invoked as a separate host process for testing; no QEMU
source or library is linked into XJ380 or shipped by the driver.

The reviewed implementation boundary is deliberately small: one driver source
file, one shared header, the existing kernel PCI/port-I/O, VFS, block-source,
mutex, scheduler, termination, and WSOD interfaces, and existing Apache-2.0
kernel support code. There is no new archive, package, generated
implementation, or copyleft dependency. The per-file inventory and SHA-256
record are in `driver/ide/SOURCE.json`; the provenance statement is in
`driver/ide/PROVENANCE.md`.

This is an engineering provenance record, not a legal opinion or a whole
project copyright clearance. Future IDE changes must review actual source
origins and update the inventory and distribution notices together.
