# Native NTFS installer formatter provenance

Review date: 2026-11-16. License: Apache-2.0.

`mkfs.cpp` and `mkfs.h` are original Apache-2.0 serialization code. They use
public NTFS format field facts from the Microsoft devnotes and public on-disk
format documentation, plus independent observations of disposable
volumes produced by the host `mkntfs` utility. No formatter implementation was
read, copied, translated or linked. Host `ntfsfix`, `ntfsinfo`, `ntfscp` and
`ntfscat` are invoked only as independent test processes on temporary images.
The formatter reuses the OS's existing FatFs uppercase mapping through an
adapter, without introducing a new linked dependency.

The bounded profile is an empty NTFS 3.1 volume with 512-byte logical sectors,
4-KiB clusters, 1-KiB FILE records, an uninitialized journal and initial system
metadata. It does not implement Windows boot-sector executable code, compression,
EFS, TxF or secure erasure. Native remount and host utility allocation/readback
tests are not Windows/chkdsk interoperability or hardware power-loss acceptance.
No patent clearance is claimed. The inventory records source provenance and
format references; it does not establish patent permission or interoperability
acceptance for public/commercial release.

The formatter is independent of the NTFS reader, writer and journal/recovery
implementation. `SOURCE.json` inventories only `mkfs.cpp` and `mkfs.h` and lists
the public Microsoft format references. No documentation prose or sample code
is redistributed. FAT32/exFAT formatting continues to use the existing FatFs
adapter.
