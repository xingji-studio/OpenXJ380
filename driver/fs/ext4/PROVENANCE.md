# Native ext4 implementation provenance

This directory contains new first-party C++ implementation code licensed under
Apache-2.0, as identified in its source headers. It was written for OpenXJ380's
freestanding environment. No external filesystem implementation, generated port,
translated source, Rust crate, or new link archive is included. The existing
kernel platform interfaces retain their own existing licensing boundary.

Implementation inputs were the public on-disk format facts, independently
formatted Linux fixtures, and existing first-party VFS/block-device interfaces.
No Linux or lwext4 implementation code is used. This is a provenance statement
for these additions, not a formal clean-room certification or a legal clearance
of the whole project. SPDX identifiers and hashes do not themselves establish
originality; review the source diff and provenance when changing the inventory.

Format references (field offsets, byte order, flags, and checksums):

- https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html
- https://www.kernel.org/doc/html/latest/filesystems/ext4/blockgroup.html
- https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html
- https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html
- https://www.kernel.org/doc/html/latest/filesystems/ext4/ifork.html
- https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html
- https://www.kernel.org/doc/html/latest/filesystems/ext4/checksums.html
- https://www.kernel.org/doc/html/latest/filesystems/ext4/attributes.html
- https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html

The parser deliberately has no write-device callback. Separate original
WriteSession, mapping, namespace, directory hash/index, attributes, orphan and
JBD2 modules implement journaled allocation, file/directory and attribute
mutations, and recovery through caller-supplied write/flush callbacks. These
modules import no external implementation code. Read, journal, and write source
files are recorded in the same source inventory. All bit extraction,
CRC32C/CRC16, extent traversal, legacy indirect mapping, directory hashing and
directory parsing/index updates were implemented here without an external
algorithm implementation dependency.
Host mke2fs/debugfs/e2fsck programs run as independent verification processes;
their implementations are neither linked into nor copied into this driver.

`directory_hash.cpp` independently expresses legacy, half-MD4 and TEA directory
hash arithmetic for signed and unsigned filename bytes. `directory_index.cpp`
implements htree traversal, collision continuation, leaf/internal splitting,
root promotion and checksums from format facts and independently generated
fixtures. Its block callbacks edit the caller's transaction overlay; enabling
LARGEDIR for indirect level two is part of the same journal transaction. Stored
root hash selectors are limited to zero through two, with signedness selected
by superblock flags. No Linux filesystem or libext2fs implementation source was
used to implement these modules.

`tests/fixtures/ext4_directory_hash_oracle.json` contains 528 numeric major/minor
hash outputs recorded from separately invoked debugfs 1.47.0 `dx_hash` commands.
`tests/fixtures/ext4_directory_hash_collision.json` records an independently found
name pair with the same major hash, cross-checked through that command. These
are output-derived numeric test data, not imported implementation source.
Linux e2fsck independently checked synthetic index layouts and confirmed that
indirect level two requires LARGEDIR; the public format documentation's level
terminology alone was not treated as sufficient verification.

Bound system-root support adds an original initial-view validation callback:
the UUID and root kernel length/CRC are checked against the EFI partition's
descriptor before the first flush, RECOVER update or replay write. Dirty mounts
use the validated final journal overlay; empty/stale logs are checked before
clearing them. Superblock write-time low/high bytes are mutable format fields,
independent of geometry, identity and feature flags. No implementation source
was imported for these additions. Boot/GPT binding and host image helpers use
first-party code and public UEFI/GPT/ext4 format facts; host e2fsprogs remains a
separate process and is not a kernel or image runtime dependency.

The original `format.cpp` / `format.h` implementation creates a bounded offline
4 KiB ext4 profile directly through caller-supplied I/O callbacks. It imports no
formatter implementation or host runtime; Linux e2fsck/debugfs only inspect its
independently produced images. Layout planning, CRCs, inode/directory creation,
JBD2 initialization and final superblock publication are expressed here from
format facts. No libext2fs library or mke2fs code is linked or embedded.
The exported `plan_format` entry computes the same layout and exact initial
free counts without invoking I/O, allowing installer budget checks before the
first destructive request. It introduces no alternate formatter dependency.

The later large-volume extension is independently authored within these same
Apache-2.0 units. Explicit `WriteStorage` and `FormatStorage` arguments retain
caller ownership instead of introducing an allocator or hidden workspace
pointers. The writer uses checked arena offsets, a 256 MiB arena budget,
generation-cleared ownership pages, sorted metadata ranges and a GDT cache
invalidated before journal writes. Bounded 1024-block recovery census batches
retain the global count of external-xattr owners, including unchanged inodes.
No upstream bitmap, sorting, cache or filesystem implementation was imported.

The formatter removes its whole-volume allocation bitmap dependency by
projecting metadata ranges into each group bitmap. Its explicit large-group
arena contains floating metadata locations and the complete GDT; the old inline
arrays remain for small callers. Public block-group/descriptor format facts and
independent Linux checks informed that layout, including small final groups and
backup descriptors. Serialized block counts remain 32-bit (at most 0xffffffff
4 KiB blocks). The separate host-image profile permits a root up to 1 TiB /
8192 groups and invokes host tools only as independent processes. The existing
source inventory remains 18 driver files; no external dependency is added.

The actual native formatter produced a 32 GiB plus 4 KiB / 257-group image that
passed Linux primary and group-243 backup fsck. Its sparse zero-write test model
only elides writes of zero into a fresh known-zero target; every formatter
callback still runs. Separate sanitizer writer tests perform small mutations
on a Linux-formatted 1 TiB sparse fixture and high-inode orphan recovery, plus
bounded host failure/replay tests. Reports are
`/tmp/xj380-large-formatter-validation.json` and
`/tmp/xj380-large-volume-host-validation-a5.json`. They do not claim a filled
1 TiB disk, a complete native format at that size, arbitrary torn-sector or
real hardware power-loss coverage. A separate completed-object probe-kernel
run exercised actual AHCI/NVMe callbacks on two 32 GiB clones, passed 44 VFS
checks, shut down through the guest, and passed Linux fsck and exact file/xattr
readback; `/tmp/xj380-large-ext4-qemu-v7q2j1df/results.json` records that narrower
VM scope. These functional checks do not constitute a license or originality
certificate.

The original `kernel/installer/ext4_import.cpp` consumes the shared PAK2 plan
using this driver's parent/target inode APIs. It retains direct hardlink
identities and opaque symlink text, applies metadata and timestamps in bounded
journal transactions, and has no allocation, VFS pathname or third-party archive
implementation dependency. Its separate kernel source review is outside the
18-file ext4 directory inventory. The trusted factory-build adapter retains one
physical lease and every heap workspace through format and import; it does not
activate an installer, publish GPT/EFI binding or promise package-wide rollback.

The later `kernel/installer/disk_layout.cpp`, `ext4_verify.cpp`,
`esp_verify.cpp` and `factory.cpp` are separate original Apache-2.0 units.
The GPT planner/serializer uses explicit on-disk fields and IEEE CRC32; the
package verifier uses this driver's read-only inode APIs. The independent
bounded ESP reader uses public FAT field facts, not copied FatFs/Linux
implementation. Its format reference is the Microsoft-authored EFI FAT
specification (Version 1.03), available at
https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf . No PDF,
specification prose or third-party implementation is embedded in these units.

The coordinator composes the existing native ext4 and FatFs implementations
under one physical lease; it introduces no alternate runtime, filesystem or
archive library. New private ESP glue and scratch-arena hooks are original
Apache-2.0 additions. Existing FatFs implementation/notices retain their
original ChaN permissive terms; `ff.cpp` and `ffunicode.cpp` are unchanged.
These additions are outside the scoped 18-file driver inventory and require
their own source, dependency, final-link and distribution review. A trusted
kernel factory API does not enable installer boot routing or validate arbitrary
physical/user-memory handoff, and does not promise atomic in-place upgrades.

The trusted VFS format adapter retains heap workspace/device references behind
a physical-extent lease. Original platform code in `driver/block_access.cpp`
and `include/block_access.h` supplies registration generations, interval claims,
static I/O tickets and explicit flush-capability declarations. Existing device,
partition, devfs and FAT glue now consume that same platform contract without
changing their published callback layouts. These are first-party additions,
not an imported interval-tree, storage or filesystem implementation. The scoped
ext4 directory inventory does not hash all platform glue; review those source
diffs and final compile/link inputs separately.

SOURCE.json records the reviewed source bytes. Both Ninja generators run
`tools/check_ext4_sources.py` to detect missing, changed, extra, or incorrectly
licensed driver inputs. The tool also restricts the isolated parser's literal
includes. This scoped inventory check does not replace a review of the rest of
the kernel's actual compile dependencies, final link inputs, or distribution.

The later boot-entry staging units `kernel/installer/boot_proof.cpp`,
`session_policy.cpp` and `session_runtime.cpp`, and the platform units
`kernel/memory/boot_memory.cpp` and `driver/block_source.cpp`, are original
Apache-2.0 code. They use public UEFI/x86 paging fields, first-party package
validation and existing device registration. The shared device-path handoff
header contains original bounded field parsing and protocol ABI declarations;
no EDK2/Linux implementation is copied or linked. The SCSI optical-capacity
fix decodes public big-endian LastLBA/count facts independently. These additions
are outside the 18-file ext4 source inventory and need separate input review.
The boot staging never enables a legacy writer or publishes a Ready source
lifetime merely because a package or registration matches.

The original `kernel/installer/source_lifetime.cpp` and `jobs.cpp` add retained
boot-source registration/lease checks and bounded opaque kernel request handles.
New private RAM-tree preparation extends existing first-party `tmpfs.cpp` and
`installer_mode.cpp` without importing another tmpfs or runtime implementation.
The real devfs capture, read-only lease query and quiescent job-reaper hooks
reuse existing kernel types and retain their ABI/layout. These are separate
original Apache-2.0 source reviews, outside the 18-file ext4 directory inventory;
no new external implementation, library, archive or distribution tool is added.
