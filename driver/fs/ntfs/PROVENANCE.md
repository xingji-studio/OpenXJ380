# NTFS P0/P1/P2/P3 and native formatter source provenance

Review date: 2026-11-17. First-party/Uinxed license: Apache-2.0; journal reference adaptations additionally retain MIT.

## Native installer formatter

`mkfs.cpp` and `mkfs.h` are original Apache-2.0 serialization code. They use
public NTFS format field facts from the Microsoft devnotes and the already
recorded format documents below, plus independent observations of disposable
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
Volume formatting and these metadata encodings join the existing release review
scope below; no new patent clearance is claimed.

## Imported portions

Repository: https://github.com/ViudiraTech/Uinxed-Kernel

Immutable revision: `ba4c809852a357378ab6058da5f514cceb8819c3`.

`format.h::decode_runs` adapts signed run-length/LCN-delta decoding from
`fs/ntfs/ntfs_runlist.c::ntfs_runlist_parse`.
`format.h::unpack` adapts the validate-all-trailers-before-restoring sequence
from `fs/ntfs/ntfs_mft.c::ntfs_record_unpack` and its layout checks.
Their upstream headers state:

> Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
> 2026/7/26 By MicroFish & JiTianYu391

The user's permission to port the friend's code was supplied in the task.
The retained public Apache-2.0 grant, rather than permission from an individual
to third-party rights they may not own, is the recorded distribution basis.
`SOURCE.json` records SHA-256 hashes of the reviewed source files and local
implementation. `UINXED_LICENSE.txt` and `UINXED_NOTICE.txt` are unmodified copies
from that exact revision. The upstream NOTICE also describes unrelated OVMF,
Limine, FatFs and font material; none of those components is imported here.

## Changes and dependency boundary

The local implementation is a bounded, freestanding C++ reader and experimental writer.
Parser, name handling, allocation, VFS integration and tests are original XJ380 code informed
by public format facts and the reviewed Apache-2.0 fragments above. P0/P1 added
partition bounds, mapping coverage, explicit limits/errors, fixed 512-byte USA
strides, independent workspaces, sequence checks and explicit hole/VDL zero-fill.
P2 adds a read-only LFS restart validator, attribute-list/extension assembly,
fragmented MFT bootstrap, the volume's own `$UpCase` table, namespace-aware name
lookup, case-sensitive directories and inactive hibernation-header checking.
`names.cpp` and the LFS validator are independently written; they contain no
ported third-party implementation. P3 adds original initialized-data overwrite, physical ownership checks,
sector read/modify/write, explicit device flush and an uncertain-I/O latch.
The metadata session adds original cluster allocation, whole-file replacement, namespace
mutations and bounded native LFS committed redo recovery, using the separately
attributed MIT journal conventions below. It adds bounded MFT/bitmap growth
preserving all prior physical extents, plus foreign LFS 1.1/2.0 analysis/redo/undo
sealed into a durable committed snapshot before home writes. The MFT-specific
attribute-list/extension growth and authenticated 512/4Kn metadata geometry are
implemented.
Ordinary-file attribute-list mutation, unbounded recovery and unresolved prepared
outcomes remain unsupported.
Ordinary Reader-only mounts have no write capability.

No Uinxed VFS, blockdev, heap, spinlock or fs_txn implementation/header is copied
or linked. No Linux NTFS/ntfs3, NTFS-3G, ReactOS, libfsntfs or dfir_ntfs
implementation/library is imported, translated or linked. No new archive is
linked. The native core includes only its own inventoried headers, `stdint.h` and `errno.h`;
the VFS glue uses existing kernel interfaces enumerated by
`tools/check_ntfs_sources.py`. `SOURCE.json` includes `names.cpp` and the public
header in the reviewed inventory. This inventory covers the NTFS additions,
not a new license audit of the entire pre-existing OS.

Host tests invoke installed `mkntfs`, `ntfscp` and related utilities as separate
processes on temporary images. Those tools/libraries are not copied into the
repository, linked into the driver, or added to the OS image. Their fixture use
grants no permission to import their implementation.

The source checker detects drift from this scoped review. It does not prove
original authorship, audit the complete kernel dependency closure, or establish
patent clearance. Every additional linked dependency needs review.

## Public format references and excluded code

`REFERENCES.json` records documentation URLs, scope and the immutable revision/hash
of the consulted libyal format document. Microsoft devnotes, public on-disk
layout documentation and primary research articles were used for factual fields,
offsets, flags and observed behavior. No documentation prose, diagrams, tables
or sample code are redistributed here. Access is not treated as a patent license.

The libyal NTFS **document** is GFDL-1.3-or-later with no invariant sections;
its license is distinct from the libfsntfs **implementation**. Only format facts
were used. Neither that document nor libfsntfs code is vendored or linked.
The linux-ntfs layout pages were also used only for on-disk field facts; their
associated GPL driver implementation was not ported.

During license screening, dfir_ntfs at
`ec3ae0884048ade106c91a68dab7c9fb3af393bf` was found to be GPL-3.0. A header,
constants and accessor names were briefly displayed before exclusion. No
function body was used or translated. The project is recorded as excluded;
the LFS parser was written from the published format descriptions, not from
that implementation. The primary research articles are separately identified
in `REFERENCES.json`; their accompanying code links are not approved inputs.

## Patent review status

P0 records a review boundary and outstanding release work, not a completed
freedom-to-operate opinion. Apache-2.0 grants only relevant rights licensable
by contributors; the friend's permission cannot grant Microsoft's patents.
Independent implementation and read-only operation do not prove noninfringement.
No claim is made that all NTFS patents have expired.

Microsoft's MS-FSCC intellectual-property notice says documentation publication
does not grant patent licenses. The reviewed OSP list does not identify an
overall grant for the NTFS on-disk format. Coverage of another protocol or
exFAT is not extrapolated to this implementation.

- https://www.apache.org/licenses/LICENSE-2.0
- https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/efbfe127-73ad-4140-9967-ec6500e66d5e
- https://learn.microsoft.com/en-us/openspecs/dev_center/ms-devcentlp/1c24c7c8-28b0-4ce1-a47d-95fe1ff504bc

Before public/commercial release, establish target jurisdictions and have
qualified reviewers map implemented features to relevant patent families,
actual claims, current legal status and license basis. The implemented feature review scope
includes volume recognition, USA, MFT/attribute lists, mapping pairs, directory
indexes, Unicode/upcase/namespace handling, clean LFS restart interpretation,
hibernation-state detection, reads, data overwrite, ownership audits and durable flush ordering. Native LFS checkpoint/OAT/transaction records, committed replay, bitmap allocation,
file growth and directory mutation are now also in that feature review scope.
MFT and bitmap extension, restart tables, tail/ring interpretation, compensation
chains, loser undo and replacement recovery commits are included as well.
MFT attribute-list rebuilding/COW relocation, extension allocation/bootstrap,
per-record attribute identities, 4Kn full-sector writes and the four-record
mirror (4 KiB or 16 KiB) are also in that review scope.
No patent numbers or clearance conclusions are fabricated by this inventory.
Compression, EFS, WOF and TxF remain absent; these limits are not a legal exemption.

## P3 scope and review additions

All new overwrite, ownership-audit, fault-model and integration code is original
Apache-2.0 code. It reuses the already attributed mapping-pairs/USA primitives;
no additional archive or runtime dependency was introduced by that data-only portion.
`REFERENCES.json` adds official storage flush documentation as factual background.
The experimental profile does not establish Microsoft's patent permission.

Both the data-only portion and a bounded native metadata transaction profile are
implemented; see `docs/NTFS_WRITE_DESIGN.md` for exact operations and bounds.
Bounded MFT growth and Windows-layout recovery are implemented; unrestricted
Windows journal recovery and full Windows interoperability acceptance remain
outstanding; bounded WinPE results are recorded separately below. Full production P3 acceptance is not claimed. Host tools and emulated durability/fault tests are not evidence of
Windows recovery interoperability or hardware power-loss behavior.

The new adapter refers to the existing native XJ380 block-sync request through
`BlockSyncRequest` in the first-party `block_access.h`. It does not add a Linux
UAPI header dependency to the NTFS implementation. The numeric ABI is unchanged;
the disposable QEMU probe statically checks it against the pre-existing alias.
This is not a new import of Linux ioctl implementation or definitions.

## P3 bounded native journal reference

The MIT-licensed LTRData/DiscUtils journal at commit `b8c77f834ad0c43baf667b1fa7a8ff42c9e360b8` supplied reviewed LFS and NTFS client layouts and replay conventions. `journal.cpp`, `journal.h`, `recovery.cpp`, `recovery.h` and
`recovery_actions.cpp` retain Apache-2.0 AND MIT attribution. The latter recovery
files adapt the pinned LFS record reader, checkpoint-table analysis, redo/undo
conventions and logical FILE/INDX operations to checked, caller-owned C++ storage.
The retained MIT author is Copyright (c) 2026, DiscUtils contributors. Source
history is not overwritten until the new full-image commit is durable; this
crash-safe recovery publication protocol and the MFT allocator are original. The C++ allocation and namespace code is original. Exact reviewed paths and SHA-256 values are in REFERENCES.json; the original root and per-file MIT notices are retained in DISCUTILS_LICENSE.txt and DISCUTILS_NOTICE.txt and installed with the image. No C# library or new archive is linked into the kernel. GPL-derived Rust journal parsers and private Windows source surfaced in search were excluded. This reference does not establish a Microsoft patent grant or freedom to operate; release review remains required.

## MFT attribute-list and 4Kn additions

The new MFT planner, reachable-record reservation, list COW, before/after overlay
validation, instance management and device-geometry checks are original
Apache-2.0 code. The journal/recovery geometry changes retain their existing
Apache-2.0 AND MIT notices. No additional third-party implementation or archive
is introduced. The first-party device header supplies existing immutable geometry;
no device/VFS ABI layout or ioctl value changes.

Official Microsoft attribute-list, attribute-header and FILE-header devnotes were
rechecked for field facts, VCN ordering, segment references and continuation size
semantics. Their sample structures/prose are not copied into the implementation.
Tests construct new records independently on separate host-tool-produced volumes.
Actual 4Kn emulated devices, USA/sector alignment and MFTMirr span are checked;
these are not claims of real Windows/chkdsk or hardware power-loss acceptance.
Microsoft's scoped patent promises are not treated as a blanket NTFS patent
license. Patent clearance remains unestablished.

A broad LFS format search automatically expanded NTFS-3G/Linux source excerpts.
No source page was opened and none of those implementations was imported or
translated. The log sequence-width boundary was independently identified by
black-box host-tool validation before that search; the local correction changes
the existing size calculation to include its endpoint. These search results
are recorded as excluded inputs, not permissive implementation references.

## Windows-produced evidence additions (2026-11-17)

Original Apache-2.0 Windows test/supervisor code runs only on disposable disk
copies in Windows 10 WinPE. The user-provided installation ISO and existing
VMs are not changed. The MSVC runtime is confined to a separate host-test agent;
no Windows runtime, SDK header or NTFS implementation is linked into XJ380.

Black-box observations found legal LFS client sequence zero, an empty resident
root `$TXF_DATA` initializer, and namespace 0 names following directory case
policy. Their parser/preservation/name fixes use observed field facts and
original regression fixtures. Retaining that exact empty initializer does not
implement TxF and does not permit active/unknown utility state or its mutation.

The corpus, isolation protocol, successful round trips and failed native-WAL
handoff are described in `docs/NTFS_WINDOWS_ACCEPTANCE.md`. A clean Windows
check is not recorded as successful journal replay when expected entries are
missing. Full desktop/Windows 4Kn and hardware crash acceptance remain open.
All existing notices, MIT adaptation attribution, GPL exclusions and patent
review boundaries continue to apply. No new implementation reference or linked
archive is introduced by this acceptance work.

The Windows-produced corpus also shows ordinary transactions terminating with
a compensation-shaped FORGET and zero undo-next, without a separate COMMIT.
The bounded parser accepts that terminal form; prepared transactions and
nonterminal unresolved undo remain refused before writes. This change uses
black-box record facts, the already reviewed MIT transaction-removal convention
and original LFS 1.1/2.0 regression/failure fixtures.

Original test-agent code now starts a bounded NTFS ETW session before a fixture
partition is published. Official ETW APIs and public symbols are diagnostic
references, separately inventoried in REFERENCES.json. Only PE/CodeView
identification metadata and public function-name/address records are examined;
Windows executable instructions, source code, private types and source lines
are not inspected, imported or translated. The stripped PDB is kept outside
the repository and image. Its symbol-server terms permit the scoped debugging
use and are not a permissive implementation license or a patent grant. No
Microsoft binary, symbol file, SDK runtime or extra archive is linked into XJ380.

## v5 Windows journal control corrections

The v5 producer keeps its original padded-page COMMIT/FORGET publication and
mirror/home flush order. Original black-box probes isolate the 40-byte minimum
client header, unused LCN sentinel, zero-data flags for empty buffers, observed
MST size hints in 512-byte units, preceding-global OPEN LSN and transaction links.
The zero-data flag interpretation also belongs to the already reviewed pinned
MIT log record reference. No further implementation source is introduced.

The earlier COMMIT probe failed because its absent redo buffer lacked the zero
flag; the corrected COMMIT passes actual Windows recovery. No claim is made
that ordinary COMMIT is unsupported by Windows. A full production transaction
now recovers in Windows before home writes, with complete namespace and hashes.
The strict native reader retains v4 compatibility and prevents broken native
COMMIT/chain records from being reinterpreted through foreign recovery.

Original Apache-2.0 host instrumentation derives fault cuts from actual I/O,
including redundant restart and FILE/INDX home publication. Failed source copies
or transport-only VM failures are not filesystem acceptance. All media, PDB,
ETW and disk artifacts remain local, outside the driver inventory and image;
the source checker explicitly rejects these artifact types in driver inputs.
Existing Apache-2.0/MIT notices, GPL exclusions and the unresolved Microsoft
patent review boundary continue to apply.

The v5 acceptance corpus also covers real Windows MFT growth, torn mirror/main
record-0 updates, and interruption of native recovery of a Windows log before
or during home publication. Expected trees/hashes and native readback are checked
after Windows recovery. Original host evidence batching and bounded offset-copy
retries handle the larger corpus without relaxing CRC/nonce/sequence checks.
Infrastructure-copy failures are retained as failed attempts, not filesystem
acceptance. No additional implementation reference, linked dependency or patent
license is introduced by these tests.
