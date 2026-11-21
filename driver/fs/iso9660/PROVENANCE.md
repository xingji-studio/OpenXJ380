# Native ISO 9660 reader provenance

The implementation in this directory and its public header are original
OpenXJ380 C++ code under Apache-2.0. They use published on-disk format facts
and existing first-party VFS, device, mutex and block-access interfaces.
No third-party filesystem implementation, translated source or new link
archive is included. The native core has only a read callback, uses no heap
allocation and includes only its own header, stdint.h and errno.h.

ECMA-119 format facts informed volume/record offsets, both-endian integers,
file-section flags, recording dates and Joliet's UCS-2 namespace. References
are recorded in REFERENCES.json. No standard text, tables, diagrams or sample
implementation is redistributed. Format-document access is not represented
as an additional intellectual-property grant.

FreeBSD cd9660 file license headers and integration interfaces were inspected
during planning. No FreeBSD algorithm, source fragment or compatibility layer
was ported. GNU libcdio was excluded during license screening; neither it nor
Linux isofs/libisofs implementation code is linked or translated here.

Tests invoke installed xorriso as a separate fixture-generating host process.
Its code/libraries are not copied into this repository, linked into the driver
or installed into the operating-system image. Synthetic fixtures and the host
and QEMU probes are first-party Apache-2.0 test code.

SOURCE.json inventories the native parser, VFS adapter and public header.
tools/check_iso9660_sources.py verifies this scoped inventory and declared
include boundary before either Ninja generator emits build edges. It detects
source drift; it is not an authorship certificate or a license audit of the
entire existing operating system. Additional linked inputs need their own
per-file provenance, dependency and license review.
