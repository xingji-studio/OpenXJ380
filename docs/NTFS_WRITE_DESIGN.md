# P3 bounded native NTFS transactions

`ntfsrw:/dev/<partition>` explicitly enables experimental writes. Automatic
probing and `ntfsro:` remain read-only, including when a log needs recovery.
NTFS remains a data-volume filesystem, not a system root or installer target.

## Implemented metadata profile

The metadata session creates regular files and directories, removes files and
empty directories, renames within or across parents without replacing an
existing destination, resizes files, and writes resident/nonresident unnamed
DATA with EOF extension and zero-filled gaps. Names use the volume's validated
UpCase table. New names are UTF-16 in namespace 0 without a DOS alias; lookup
follows the directory case policy. A rename removes the old DOS alias.
Allocation bitmaps, mapping pairs, VDL/EOF, FILE_NAME values and directory index
keys are updated together. Small indexes become resident; larger supported
indexes use a bounded native $I30 B-tree and an allocation bitmap.

The metadata geometry uses 4096-byte clusters/index buffers and either
1024-byte or 4096-byte MFT records. Logical sectors can be 512 bytes (including
512e) or 4096 bytes (4Kn); a FILE record must cover at least one logical sector.
The kernel supplies authenticated physical-device geometry and refuses a BPB
sector-size mismatch before any write, including before legacy fallback. All
metadata writes cover complete logical sectors. USA protection retains its
format-defined 512-byte stride. The mirror covers the first four FILE records:
4 KiB for 1 KiB records and 16 KiB for 4 KiB records. Its entire mapping and
allocation are checked and protected, and changed mirror records are flushed
before any changed primary record.

Free initialized MFT slots at or above 24 are reused with a new nonzero
sequence. Growth initializes through the next cluster boundary, using allocated
spare capacity first and appending a cluster when needed. Every new unused slot
receives a valid empty FILE header. Existing physical MFT/bitmap extents are
preserved. DATA mapping pairs, allocation/EOF/VDL, MFT and volume bitmaps, FILE
records, directory entries, attribute lists and MFTMirr share the same transaction.
The MFT bitmap grows in eight-byte units and can become nonresident.

### MFT attribute-list and extension growth

Resident and nonresident MFT ATTRIBUTE_LIST forms are supported, with unnamed
DATA/BITMAP fragments and a resident bitmap in an extension record. The planner
validates every referenced segment, preserves base references and sequence
numbers, and keeps the initial DATA fragment in the base record for bootstrap.
Continuation size fields are canonicalized to zero; their VCN ranges and LCN
deltas remain exact. If a tail runlist no longer fits, it can move to an already
reachable segment or keep its old range and add a new continuation segment.
Non-DATA/BITMAP values survive unchanged, including when a retained field must
move out of a full base record. Attribute instances are unique in each segment,
and the list is rebuilt in type/name/VCN order with updated identities.

New extension records use unallocated, already initialized records: 16..23 are
preferred over ordinary free slots. A new DATA extent's owner must be reachable
through the preceding known map. Once that reserve is consumed, a final free
initialized user slot is retained while the next cluster is initialized. This
avoids a self-dependent MFT bootstrap. If a foreign volume has no reachable
free segment and the old runlist cannot fit, growth fails before journal/home
writes; physical addresses are never guessed. Empty moved-out extension records
are retired and are not reused for a user file inside that same transaction.

The attribute list can remain resident or use a bounded contiguous COW stream;
an existing nonresident list is replaced transactionally and its old allocation
is released in the same decision. The list is at most 64 KiB/1024 entries and
must have its complete descriptor in the base record. New list storage requires
a contiguous free range of at most 16 clusters. Insufficient bootstrap space,
record space, mapping capacity or log space returns ENOSPC/EOPNOTSUPP without
publishing a partial plan.

Validation reconstructs both before and after filesystem views from WAL images,
so it also works after partial home writes and during repeated recovery. It
checks prefix preservation, growth bounds, unchanged non-growth values and every
live stream destination. General ATTRIBUTE_LIST mutation for ordinary files and
directories remains outside this profile.

Other limits are 64 metadata page images per transaction, 64 directory keys
(including aliases and the root self-key), 12 KiB of directory key entries,
up to four index leaves plus an internal buffer, two edited parents, 65536
initialized MFT records, a 1 MiB cluster bitmap (up to 32 GiB with 4 KiB clusters),
1024 mapping runs per stream, 16 MiB per changed file and 1 MiB per pwrite.
Bounds and unsupported record forms fail before journal/home writes; newly
written unreachable file clusters can remain in free space after a data error.

Hardlink mutation, destination replacement, open-file unlink/orphans, case
sensitive directory mutation, ordinary-file ATTRIBUTE_LIST mutation, object IDs, reparse,
compression, EFS, WOF, TxF operations and sparse-stream modification are refused.
The observed empty resident root `$TXF_DATA` initializer is allowed only when
its root reference, size and empty-state signature match; it is preserved byte
for byte through all native/foreign metadata validation. Nonempty or unknown
utility state and any logged update of that initializer are refused. ACLs,
quotas, USN/change-journal maintenance and WSL ownership are not implemented.
The driver does not claim Windows-equivalent security or change notifications.
New files reuse the parent's stored security identifier under the existing local
uid/gid-0 policy; writable ordinary files/directories use 0644/0755. It is intended
for disposable experimental data volumes, not Windows system volumes.

Unsupported metadata geometries/log versions may use the separately audited
initialized-nonresident-data overwrite profile after a strict clean read-only
mount succeeds. That profile preserves metadata and has no data atomicity
promise. It never recovers a dirty or unknown journal. Its original 1 MiB/65536
record/4194304-comparison bounds and system/boot/mirror/ownership checks remain.

## Native journal and ordering

The writer emits native LFS 1.1 pages and NTFS client 1.0 records, using a
checkpoint, OpenNonresidentAttribute records, complete FILE images
(InitializeFileRecordSegment), nonresident byte updates for INDX/bitmaps/attribute lists,
CommitTransaction and ForgetTransaction. Each metadata update carries redo and
undo bytes. There is no private WAL, footer, checksum or hidden recovery file.

The log must have a single base nonresident DATA attribute, be fully initialized,
and be 1..64 MiB. The transaction reserves at most 160 4096-byte data pages,
beginning after two restart pages and two tail pages. Record data is padded to
whole LFS pages; exact reservation accounts for deallocation-only undo of newly
initialized 4 KiB records; native lengths delimit the actual NTFS redo/undo data. The LFS sequence width includes the file-size endpoint, including power-of-two
log lengths. Older prototype widths remain readable, and the next committed
transaction emits the canonical width with a newer LSN epoch. Sequence
bits, client identity, LSN chains, page fixups, transaction termination, target
streams, nonoverlap and physical bounds are checked. This emitter's committed
profile has a strict direct replay path. Foreign logs use the separate analysis,
redo/undo and snapshot publication path below. The 160-page reservation can
reject a transaction before the 64-image bound is reached.

The v5 emitter retains a 40-byte minimum NTFS client header and an unused LCN
sentinel for unmapped control records. It marks empty redo/undo buffers with the
LFS zero-data flags, exempts compensation markers from ordinary undo buffers,
emits observed MST hints in 512-byte units, records the preceding global OPEN
LSN and starts the metadata transaction with a zero previous-LSN. COMMIT and
FORGET terminate their undo-next chains. The strict reader retains v4 control
record compatibility. Once the padded native profile is recognized, malformed
native commits/chains are refused without a fallback to ordinary foreign FORGET
semantics. These corrections change record conventions, not flush/home ordering.

The serialized protocol is:

1. Check every initialized MFT header against its bitmap, require the twelve
   mandatory system records to be allocated, and scan every allocated segment,
   including extensions and named streams,
   to validate bitmap ownership and detect crosslinks. Protect the BPB, backup
   boot storage, MFT mirror and log independently of mutable data mappings.
2. Reserve all metadata images, names, directory/index/bitmap space and free
   data clusters. Construct and validate the complete staged filesystem view.
3. For a changed nonresident file, write a replacement stream to free clusters,
   preserving old contents and explicitly zeroing gaps/tail bytes. Flush it
   before any commit decision; the old stream remains reachable until commit.
4. While the prior log is clean, clear and flush the reusable bounded log window.
   Write all records, including redo, undo, COMMIT and FORGET; flush the log.
5. Publish a dirty restart to the alternate copy and flush, then the source copy
   and flush. No home metadata write precedes these durable decisions.
6. Write any changed first-four MFT records to MFTMirr and flush before their
   primary copies. Apply every home image, then flush all home metadata.
7. Publish and flush the alternate clean restart, then the source clean restart.
   Only then report a successful metadata operation.

A new all-FF log is first given redundant clean restarts. Interruption during
its very first restart is recognized only when slot zero and all data pages
are FF and slot one consists solely of FF/canonical initial-clean image bytes.
The complete filesystem is validated before repairing that initialization.
Other double-restart damage is never interpreted as a fresh log.

## Recovery and failure contract

Recovery bootstraps the immutable $LogFile record from the fixed first four
MFT records or their mirror, so a torn mutable MFT prefix need not prevent
finding the log. It selects a valid newest restart by LSN and revision; one
invalid copy can be repaired. Recovery first reconstructs a read overlay of the
committed images and validates the volume, hibernation header, ownership,
physical stream mappings, live MFT bits, touched directory trees and matching
FILE_NAME/index keys. Only then does it write any recovery metadata. Redo is
idempotent; the dirty log remains authoritative until all home images are
flushed. Power loss during recovery can repeat the same protocol.

Uncommitted transactions from this emitter have no home writes and keep the
prior restart clean. Undo bytes are still emitted in its native NTFS records.

### Foreign Windows-layout logs

The separate reader supports LFS 1.1/2.0 with 4096-byte pages and one NTFS
client, and NTFS client checkpoint versions 0/1. It reads packed records,
page-spanning records, a wrapping log ring, valid newer tail copies, and records
newer than RSTR.CurrentLsn. It loads checkpoint open-attribute, attribute-name,
dirty-page and transaction tables, then analyzes subsequent records. Retained
pre-checkpoint redo and loser histories are followed through their actual LSNs.

Redo uses dirty-page mappings and page LSNs; undo follows active transaction
chains and compensation undo-next pointers. Checked actions cover FILE
initialization/deallocation/end/zero, attribute create/delete/resident values,
mapping pairs and stream sizes, index-root/allocation entry add/delete/end,
child pointers, duplicate FILE_NAME information, index record data, nonresident
bytes and bitmap bit ranges. Unknown operations, relative-index operations
0x23/0x24, unresolved prepared transactions, missing history, remapped undo
clusters and ambiguous overlapping undo are refused before disk writes. Reused
attribute-table identities with retained older changes are also refused. No
TxF recovery engine is provided.

An ordinary transaction can end with a FORGET record whose undo operation is
the compensation marker and whose undo-next LSN is zero, without a separate
COMMIT record. A preceding nonempty undo chain does not make that terminal
record an unresolved loser. A nonterminal FORGET with outstanding metadata undo
still returns EUCLEAN; a prepared transaction still returns EOPNOTSUPP before
any backend write. Independent LFS 1.1/2.0 fixtures exercise these distinctions,
including repeated failure cuts during recovery.

Recovery is bounded to 4096 scanned records, 128 attribute identities, 128
transaction identities, 512 dirty-page entries, 32 clusters per log target,
512 checked namespace parents, 192 KiB per client record, 64 KiB restart-table/operation storage, 16 UTF-16
units per attribute name, and the metadata image/reservation limits above.
Per-file MFT undo owners and per-bit bitmap ownership reject conflicting loser
updates while allowing separate FILE records and disjoint bitmap bits.

All redo and undo are first applied in memory. The resulting overlay must pass
the full allocation/MFT audit, exact live destination-stream mapping checks,
log-map immutability check, hibernation checks and touched directory/FILE_NAME
checks. Old and new FILE_NAME parents are checked even without a logged
directory image, and every changed live FILE_NAME must have its exact index
entry. Cached duplicate size/timestamp information may be stale in NTFS; the
validator checks reference, parent, namespace and stored name association.
FILE and raw-MFT changes share full physical MFT-cluster snapshots, preserving
neighboring records. Any changed MFT prefix is mirrored before its primary.

A full 160-page contiguous replacement window must exist outside **every**
retained source log page and tail slot. Otherwise recovery returns ENOSPC
without writing. The original recovery history stays intact while this gap is
cleared/flushed and a new native full-image COMMIT/FORGET is written/flushed.
Its LSN epoch exceeds the source history. Only after both replacement dirty
restarts are durable can home writes begin. A crash before that decision
repeats the original Windows-layout analysis; a crash after it repeats the
native committed snapshot. Only flushed homes permit clean restart publication.
This supplies persistent undo results without overwriting the source history
or relying on an unlogged home-page rollback.

Successful log recovery does not clear the NTFS volume dirty flag. If it
remains set, recovery may finish durably but the writable mount still returns
EUCLEAN and leaves that flag for Windows consistency checking. An unknown log,
unsafe hibernation state or unsupported recovery request is never cleared to
force a mount.

A failed read/write/flush latches the session. Kernel owner death or uncertain
I/O poisons the retained block lease and keeps all relevant objects until reboot.
Retry/read/sync/unmount requests cannot resume the failed mount. Successful
flushes must mean actual device-cache persistence; dishonest flushes, arbitrary
misdirected writes and independent raw writers are outside the fault model.
Filesystem leases pin generations and exclude overlapping filesystem mounts;
administrators must exclude other raw writers and live Windows attachments.

Whole-file replacement can be old or new after recovery, including when the
original call returned EIO. Namespace mutations follow the same commit rule.
An interrupted directory/file operation must be recovered through explicit
ntfsrw before ordinary read-only mounting. Legacy overwrite-only sessions retain
their previously documented old/new/torn data behavior without metadata changes.

## Verification and release status

Host tests use the actual freestanding sources with ASan/UBSan, disposable
mkntfs images and independent ntfscat readback. Fault tests cut every write and
flush, retain or lose volatile writes, recover and compare complete user trees,
and repeat cuts during recovery. A separate pinned MIT DiscUtils parser/replayer
checks native dirty-log records without linking any C# code into the OS.
The QEMU runner exercises actual AHCI/NVMe VFS paths on disposable images,
including an NVMe device that reports 4096-byte logical/physical sectors and
GPT LBAs in those units. QEMU 6.2's IDE model requires 512-byte logical sectors,
so the AHCI path retains its 512-byte regression coverage. The probe authenticates
both geometries and checks every observed backend write's alignment.
Tests include full extension/base records, continuation creation with no reserved
slots, resident-list conversion, bitmap relocation, MFT/bitmap growth, all write/
flush cuts and repeated recovery. A separate reference parser checks both native
4Kn growth logs and constructed foreign 4Kn redo/undo results.

Windows 10 WinPE-created volumes now pass production-core and kernel VFS write
round trips, with Windows content checks, complete SHA-256 manifests and readonly
`chkdsk` returning zero. One actual Windows LFS 1.1 crash sample recovered to
the same complete namespace/content as Windows itself, then passed Windows
`chkdsk`; all 428 write/flush failure cuts and 428 repeated-recovery cuts passed
under ASan/UBSan. These tests found and fixed zero-valued LFS client sequences,
the passive root utility initializer and namespace/case handling.

The v4 native dirty-WAL handoff omitted a committed create in Windows despite
a clean `chkdsk` result. v5 fixes the control record conventions and passes the
actual production-core gate, with every expected directory entry and SHA-256
matching. Eight selected restart/home/flush failure samples also pass Windows
readonly `chkdsk`, full comparison and native readback. On the same Windows seed,
406 native write/flush cuts and 22 recovery recuts pass under ASan/UBSan.
Two additional cases interrupt native recovery of the actual Windows crash:
after replacement publication and halfway through its first home write.
Windows then recovers the complete 30-entry oracle tree and passes readonly
`chkdsk`; native readback agrees. Source Windows history remains protected until
the replacement publication decision in these tests.
On a real Windows-created volume, the kernel's 272-file growth workload extends
MFT initialization from 256 KiB to 512 KiB through both AHCI and NVMe. Windows
then checks all 285 returned entries and hashes, with both readonly `chkdsk`
checks returning zero. Two more cases tear the first growth MFT mirror or primary
record-0 write; Windows and native readback recover the same 221-entry committed
tree. These are 512-byte Windows volume results; Windows-produced attribute-list
and 4Kn growth corpora remain outside the evidence below.
The MIT parser remains an independent format check. Full desktop Windows, actual Windows 4Kn logs,
real LFS 2.0 crash corpora and hardware power-loss behavior are still unverified.
See [NTFS_WINDOWS_ACCEPTANCE.md](NTFS_WINDOWS_ACCEPTANCE.md) for the exact
environment, isolation protocol and remaining production P3 acceptance.

First-party code and Uinxed fragments retain Apache-2.0 attribution. The native
journal and foreign recovery adaptations of pinned DiscUtils additionally retain MIT notices. All
exact references/hashes are in SOURCE.json/REFERENCES.json; licenses and provenance
are staged under /system/licenses/native-ntfs. No GPL implementation is imported
or linked. Separate host mkntfs/ntfscp/ntfscat processes are fixture/readback tools.

Public format documentation and permissive source licenses do not establish a
Microsoft patent grant. The Microsoft Open Specifications Promise is scoped to
its listed specifications and necessary claims; coverage of this on-disk journal
has not been established. See PROVENANCE.md for release review requirements and
Microsoft's [patent/program information](https://learn.microsoft.com/en-us/openspecs/dev_center/ms-devcentlp/13571077-e344-4e6f-a477-369894979798).
