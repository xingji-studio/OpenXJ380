# Native ext4 integration status

The original Apache-2.0 driver supports **journaled writable ext4 volumes**
through ordinary VFS reads, writes, file/directory creation, deletion, growth,
truncation, and same/cross-directory rename with replacement. Creation records
mode, uid and gid in the directory/inode transaction. Writable mounts recover
supported committed journals before publication. An explicit source such as
`ext4ro:/dev/sata1p1` mounts read-only and never changes or replays the device;
a volume needing recovery is refused in that mode.

This remains a supported subset, not a complete ext4 implementation. System-root
mounting uses the explicit boot binding described below; ordinary application/data
mount APIs still refuse the system root. The public image builder accepts `VDISK_FS=ext4` or `--fs ext4` for
an explicitly bound writable root and FAT32 ESP. FAT32 remains the default image
format. A trusted kernel factory API now builds a fresh ext4 root and FAT32 ESP
on a whole disk. The versioned native graphical installer supports fresh,
full-package ext4 installation with a FAT32 ESP. Legacy copy/repair entrypoints
remain refused; repair, keep-users and in-place update are not enabled.
Existing installations are not converted automatically. Hard-link and fast/mapped symlink creation are supported. Inode-body
and external-block xattrs have get/list/set/remove APIs, including Linux path,
no-follow and descriptor syscalls. Existing sparse and unwritten data is preserved.

The shared installer package validator now checks complete PAK1 trees before
installer-root preparation, target precheck and boot-file repair. The host
encoder retains PAK1 by default and offers explicit `--format pak2` for richer
metadata and link identities. Existing installation copy paths refuse PAK2
because they do not implement those semantics. A separate original Apache-2.0
inode-relative importer and trusted `ext4fs_build_node` API format and populate
a fresh offline ext4 partition from PAK2. The separate trusted
`installer_build_ext4_disk` coordinator consumes PAK2, constructs GPT/root/ESP,
verifies the complete result and publishes the boot binding last. This does not
enable the legacy writers; the versioned native installer uses this coordinator.
See [the package contract](INSTALLER_PACKAGES.md).

## Writable feature and durability boundaries

- Writable sessions share a 32-entry clean read cache for home blocks and
  journal images in caller-owned workspace storage (128 KiB data maximum).
  Overlay after-images take priority; every attempted write invalidates its
  affected cache blocks first. A cold read failure also blocks subsequent warm
  cache reads and mutations. Large/cross-block reads retain direct backend I/O;
  explicit read-only mounts retain their existing uncached parser behavior.
- Journal log-data, checkpoint and replay writes combine physically consecutive
  blocks into requests of at most 16 blocks / 64 KiB, using an explicit backend
  opt-in and a caller-owned gather buffer. Fragmented mappings split requests.
  All six commit flush barriers, checksum/escape handling and pre-write recovery
  validation are retained. No delayed dirty-cache writeback is introduced.
  A synthetic 16-image contiguous commit drops from 37 write requests to seven;
  that is a request-count result, not a hardware throughput measurement.
- Tested filesystem blocks: 1/2/4 KiB. The fixed 128-block-group writer limit is
  removed: larger geometries use explicit caller-owned `WriteStorage`, sized
  from block, inode and group counts. Checked arena admission has a 256 MiB
  budget; this is not a bound on all memory used by a mount. Small direct
  callers may retain the inline fallback. Allocation can initialize unused
  group bitmaps in the same transaction as their first use.
- Extent-tree updates support depth zero through five, node splits, root growth
  and collapse, and unwritten conversion; tests force depth two. Classic direct,
  single/double/triple indirect maps support sparse allocation and reclamation.
  Linear directories may grow up to 128 blocks. Indexed directories retain their
  htree structure during create, unlink and same/cross-directory rename, including
  replacement, hash collisions, leaf/internal splits and root promotion. Htree
  indirect levels zero through two are supported, within the native limit of
  4,194,304 logical blocks. Promotion to level two atomically enables LARGEDIR in
  the same journal transaction; that feature is never cleared. A full one-block
  linear directory can convert to an index when DIR_INDEX is already enabled.
  Legacy, half-MD4 and TEA hashes support signed and unsigned filename bytes
  (six arithmetic variants). Disk root selectors remain zero through two;
  superblock flags select signedness. Unknown selectors or conflicting flags
  are refused before writes.
  Symlink text is limited to one filesystem block minus its terminating NUL.
- Each complete journal transaction contains at most 16 block after-images,
  including data, inode tables, bitmaps, group descriptors and the superblock.
  Large writes may return a committed prefix; they are not atomic as a whole.
  Namespace operations commit once or fail before device writes when metadata
  exceeds that budget. Truncate first tries one transaction, then uses a durable
  final-size intent and traditional orphan chain for bounded tail reclamation.
  Last-link unlink and rename replacement atomically remove/replace the name and
  add the victim to that chain, then reclaim data, mapping blocks, xattrs and the
  inode in bounded transactions. Large reclamations need not fit one transaction.
  Writable mount validates and finishes interrupted linked-file truncates and
  supported zero-link orphan deletion before publication; explicit read-only
  mount refuses pending orphans. Malformed chains and still-named zero-link
  victims are refused before recovery writes.
- Growth may create sparse holes. Partial-block shrink/extension clears stale
  tail bytes; newly allocated blocks are initialized before publication.
- An open file survives unlink or rename replacement with its inode identity and
  data available through retained descriptors. Recreating its old name creates a
  separate inode. The zero-link victim remains on the durable orphan chain until
  its final VFS reference closes; close reports a reclamation failure and failed
  I/O locks out further mutations. A live unlinked reference keeps unmount busy.
  An unlinked held directory can be read, but cannot acquire new children.
  External xattrs use checksummed blocks, shared-block copy-on-write, reference
  accounting and last-reference reclamation. EA_INODE value storage is refused.
- Creation and chmod/chown/utimens persist mode/uid/gid and timestamps through
  journal transactions. Extended inodes retain nanoseconds and epoch bits, with
  signed seconds from -2^31 through 0x37fffffff; short inodes reject times they
  cannot represent. Xattr create/replace semantics, binary/empty values and size
  queries are supported. Trusted attributes require privilege, protected
  namespaces require privilege to modify, and user xattrs on symlinks are refused.
  VFS system/ACL mutations and unknown namespace mutations return EOPNOTSUPP;
  imported values remain readable through native metadata APIs. Stored
  capability attributes do not imply enforcement by this kernel.
  Mutation APIs require task context and heap-owned storage.
  File mappings support private snapshots only; shared mappings are refused
  until coherent dirty-page writeback is implemented.

The original JBD2 implementation supports fragmented internal journals of at
most 32768 logical blocks, checksums v2/v3 or no checksum, 32/64-bit tags, magic
escape, multi-descriptor and multiple pending transactions, revoke records and
sequence wrap. Recovery builds and validates the final surviving overlay before
any home write; a heap-owned index allows more than sixteen recovery images.
Checksum v1, async commit and fast commit are refused without clearing them.

A transaction's complete after-state is checked through an overlay before any
log/home mutation: geometry and reserved journal storage remain fixed; bitmap,
free-counter, descriptor, inode and directory checksums/relationships must be
consistent. Recovery can bootstrap immutable journal locations through a torn
superblock/group checksum and then strictly validate the complete replayed
state. Read-only mounts never use that bootstrap path. An offline Linux fsck is
required before using imported volumes; these checks are not an authenticity or
complete inode-ownership audit of arbitrary malicious disk images.

A clean writable mount checks flush support before setting RECOVER. RECOVER
remains set throughout the active writable mount; successful unmount flushes and
clears it before freeing the cached tree. Failed I/O locks out mutation and
makes fsync/unmount report an error; failed cleanup must not claim a clean disk.
The VFS preserves callback errors, hides pending creations, rolls back failed
cache entries, and retains managed mount references through lookup/unmount.
Cached aliases receive updated inode metadata after successful mutations.

If a terminated task abandons the ext4 operation guard, lock recovery waits
until the scheduler retains no current, selected or retired reference to that
task. The adapter then permanently fails all ext4 operations with EIO until
reboot. Mount/device/WS storage is quarantined; cleanup does not resume the
partial in-memory transaction, reclaim its buffers, clean-close the writer or
clear RECOVER. Journal recovery after restart remains necessary. The host guard
test simulates task abandonment and address reuse; it does not certify device
DMA cancellation or recovery of partially published in-memory vnode state.

AHCI/NVMe issue real device flush commands and validate command completion,
protocol errors and bounded timeouts. AHCI halts after disabling bus mastering
if DMA engine shutdown cannot be confirmed; NVMe retains DMA buffers and takes
an ambiguous controller offline. Host crash models include every write/flush
prefix in write-back/write-through and failed-I/O-persisted/not-persisted modes.
They do not guarantee arbitrary torn sectors, silent device corruption, or
hardware that violates flush semantics.

## Native offline formatting, import and device lifetimes

`ext4fs_format_node` is a trusted kernel API, not a user-pointer or raw-device
syscall. Its caller retains a real devfs block node and supplies a nonzero UUID,
creation time, 4–32 MiB journal size and optional sixteen-byte label. The adapter
requires ordinary task context with scheduling/interrupts enabled and checks
mount/write permission before entering storage privilege. The whole-disk factory
uses the corresponding borrowed-window build API; the legacy installer paths
have not adopted it.

The original formatter writes 4 KiB blocks, 256-byte inodes, 64-byte descriptors,
8192 inodes per group and a minimum 32 MiB volume. Above 128 groups it uses an
explicit caller-owned `FormatStorage` arena for metadata locations and the
complete group descriptor table. Floating metadata locations also support
inode tables for small final groups. Planning projects metadata ranges onto
each group bitmap instead of keeping a bit for every physical disk block.
The serialized formatter geometry still uses 32-bit block counts: its upper
bound is 0xffffffff blocks (16 TiB minus 4 KiB), not an arbitrary 64-bit volume.
The writer's separate arena budget can reject a smaller geometry. Formatting
initializes every inode table and journal block. Its factory features are
has_journal, ext_attr, dir_index, filetype, extents, 64bit, flex_bg, sparse_super,
large_file, huge_file, dir_nlink, extra_isize and metadata_csum. It does not
advertise online-resize reserve space or lazy initialization. Existing import
and runtime feature support remains separate from this factory profile.

The pure `xjext4::format_storage_size` and `xjext4::plan_format` preflights compute
arena requirements, exact factory geometry and free block/inode counts without
invoking device callbacks. The large-volume plan and formatter receive the
same retained arena; `format_group_location` exposes every group's locations
without assuming that the legacy 128-entry arrays contain the whole layout.
`ext4fs_build_node` validates the complete immutable PAK2 and its conservative storage budget before
formatting, then retains one exclusive physical extent lease through format and
import. All session/map/workspace storage and the device pin are published before
the first backend request. The caller must retain mapped, immutable kernel
package storage throughout the operation, or until reboot after quarantine.
This trusted kernel interface does not accept arbitrary user pointers.

`ext4fs_build_window` instead synchronously borrows a caller-owned
`BlockAccessWindow` under one exclusive real whole-device lease. Each callback
checks the current TCB/TID, lease, physical registration generation, separately
resolved devfs node and real 512-byte logical-sector geometry, then checks local
bounds before translating to whole-device offsets. Flush targets the physical
whole device. Slices, stale identities, shared/scan leases, poison and 4Kn
devices are refused. Ordinary overlapping claims remain EBUSY even for the same
owner; a window creates no sublease and never releases its parent's lease.
The caller retains the node, parent token and immutable mapped payload through
success, or until reboot after quarantine. This is a same-TCB kernel borrow,
not a raw mapping or asynchronous/user-pointer interface.

The importer accepts only a fresh native 4 KiB/256-byte-inode volume. It checks
the factory features, initialized allocation metadata, empty root and
`lost+found` before the writer's first flush/write, including through the
WriteSession initial validator. Parent/target inode operations preserve opaque
symlinks, forward hardlinks, exact data, ownership, final mode, supported
`user.*` xattrs and nanosecond mtimes without following package link text.
Conservative block budgeting is not a reservation. A package is not one atomic
transaction: interruption can leave a committed prefix. Every error after
writer start retains the complete lifetime without close/retry or clearing
RECOVER; backend failure stops all further I/O. Successful completion alone
closes the writer and releases resources. Existing-target repair and keep-users
remain separate policies.

Formatter parameter/layout validation precedes I/O. A real flush preflight precedes
the first destructive write. The formatter invalidates primary/new-profile
backup supers, initializes complete metadata and journal, flushes them, then
publishes VALID backup supers before the VALID primary and final flush. A
failed final publication request can still persist a complete VALID filesystem;
phase flags describe completed/attempted phases and do not imply a failed write
could not land. First backend error ends all I/O. This is formatting metadata,
not secure erase or invalidation of every historic backup signature.

The adapter heap-allocates its 537600-byte format workspace and any required
format/writer arenas, and pins device/context storage before the corresponding
operation's first request. Pure geometry admission and allocation failure do
not start a native format/import write. A successful close releases the writer
arena; a failed or abandoned I/O lifetime retains it with the quarantined
context. Device identities include registration
generation and physical byte range. Mounts reject overlapping filesystem-cache
aliases; formatting and scans take exclusive range leases. Whole-disk/slice
aliases, stale devfs generations, in-flight requests and slot exhaustion cannot
bypass these checks. Different non-overlapping partitions can remain usable.
Cached devfs handles never substitute a reused device ID's new generation;
raw block mappings are refused without a mapping-lifetime protocol.

Writable ext4 mounts and native formatting require an explicit immutable real
flush capability from the physical backend. AHCI writable ATA, NVMe and IDE
register that capability; partitions inherit it through the physical generation.
An unknown backend returning zero for an ioctl does not establish support and
is refused with EOPNOTSUPP. Explicit read-only ext4 mounts do not require flush.
FAT formatting and clean-unmount preparation use the same capability check.

A format I/O failure poisons its extent and retains workspace, device pin and
lease until reboot, including failure of the initial flush. Quiescent task exit
poisons abandoned exclusive leases and static tickets before TCB reuse. A live
abandoned I/O ticket triggers global block I/O refusal because a backend mutex
or DMA request may still be owned; this does not cancel already submitted or
waiting requests. FAT uses a recoverable guard too: abandonment permanently
rejects its callbacks, retains its caches/leases and never resumes partial work.

FAT opts into VFS busy/traversal retirement during unmount without adopting
managed-file or symlink semantics. VFS first evicts owned link-cache references,
checks external references, and checked-closes child backend handles while the
tree remains intact. FAT's successful final-flush preparation then retains its
recursive mutex through storage disposal and final teardown. Optional checked-unmount
callbacks report late errors while preserving the failed mounted context and
source lease; VFS does not publish a successful unmount on those errors.
Filesystem-probe errors are retained through later mount probes, so a transient
publication EBUSY is distinguishable from EIO. Callers may retry EBUSY only while
the mountpoint remains unpublished; I/O failure is not a retry signal.

Legacy link caches now own references independent of the lookup result,
including first lazy follow and competing publication. Checked subtree
retirement prevents new traversal/publication, detaches cache ownership under
the child lock, and closes targets after releasing that lock and any FAT mutex.
It rechecks external references before backend close, namespace removal and
storage disposal. Directory refresh uses this contract rather than freeing a
referenced subtree. Backend errors retain the tree/context and propagate to
the caller; genuine open references still keep unmount busy.

Lookup pins intermediate nodes and captures/pins `..` or a relative link's
parent under the same child lock. Relative text-link results are resolved on
each lookup instead of retained in a path-based cache whose parent ancestry
may change. Legacy link rename retains both parents, checks membership and
collisions, and protects the source while evicting its old cache outside locks.
Link/path buffers are heap-owned and bounded to 4095 bytes; oversized paths
and link recursion are refused explicitly.

The bounded protocol has 64 concurrent subtree-retirement records and 64
compatibility detached-tree retention records. Exhaustion refuses retirement;
detached-retention overflow retains storage and prevents clean unmount until
reboot. A collector terminated during disposal may also retain its unfinished
state; arbitrary interrupted cleanup is not resumed. The legacy void free API
does not silently consume a referenced tree or release cross-volume cache
targets under an unknown caller lock.

Procfs uses the same checked retirement and stable process-observation tickets.
Retained nodes return ENOENT after process collection without retaining or
dereferencing its freed PCB. FD snapshots/readlinks retain descriptor-operation
tickets, and dead-owner cleanup drops those tickets before process observations.
Procfs is a read-only bootstrap singleton with private publication and checked
closure of the old mountpoint backend. Concurrent live exec/VMA content
snapshots and arbitrary interrupted collectors are outside this protocol.
See [the procfs lifetime contract](PROCFS_LIFETIME.md).

## Native whole-disk factory

`installer_build_ext4_disk(target, source, options, package, bytes, status)` is
an implemented trusted synchronous kernel API for destructive fresh-disk
construction. It excludes the caller-supplied source's physical identity and
holds one exclusive target lease across GPT, native ext4 format/import, private
FAT32 ESP construction, verification and binding publication. Target geometry
must have real 512-byte logical sectors; the root and ESP use authenticated
windows of that same whole node. No temporary partition registration or VFS
mount publication is needed. The caller must identify the actual source medium;
supplying an arbitrary source node does not authenticate an installation request.

Before target I/O, the coordinator validates the complete PAK2, required
kernel/EFI/config files, identities, GPT layout, native ext4 geometry and both
payload budgets. The shared pure `esp_plan` checks FAT32 geometry and payload
cost without allocation or device callbacks. `ext4fs_factory_ready` and
`fatfs_factory_ready` reject known uninitialized, failed/shutdown or recursively
owned subsystem state without I/O or retaining a guard; these observations do
not reserve future availability. The coordinator refuses an existing storage
privilege scope. Each layer publishes all of its heap/workspace state and device
pins before its first backend request. Root and FAT guards are used sequentially.
The writer's arena is size-checked in factory preflight but allocated later by
the root builder. That allocation can fail after the factory has withdrawn the
old boot state or written GPT. The destructive fresh-install contract then
quarantines the context and leaves the new binding inactive; it does not promise
that every later allocation failure performs zero target writes. The factory
regression explicitly exercises this late-ENOMEM path.

After a real initial flush, the factory withdraws the old MBR and both GPT
headers, flushes and verifies that withdrawal, then clears the entire new ESP
and flushes it before publishing the new GPT. Reused caller GUIDs therefore
cannot expose an old valid binding while new payload construction is incomplete.
This sequence destroys the old installation; it is neither secure erase nor an
atomic upgrade. The factory writes and checks both new GPT copies, builds the
root, and constructs the minimal ESP with a 96-byte all-zero binding placeholder.

The independent read-only `ext4_verify_package` gate walks the complete manifest
by parent inode without following symlinks. It checks exact names, canonical
inode/hardlink identities, regular and symlink bytes, uid/gid/mode, nanosecond
mtime and all represented `user.*` attributes, including root metadata. Extra,
missing or duplicate names and unexpected inode aliasing are refused; an omitted empty factory
`lost+found` inode 11 is the only permitted extra. It also checks the expected
UUID, native factory profile, clean journal, clear RECOVER/orphan state and
allocation metadata checksums/counts. This is not a general data-block ownership
fsck or an authenticity check of arbitrary malicious media.

A separate read-only FAT32 parser checks the minimal ESP's complete directory
and allocation graph, mirrored FATs, BPB/FSInfo/backup metadata, exact EFI/kernel
bytes and zero placeholder. It proposes a single publication sector without
writing it. Only after both writers have closed, full root/ESP readback has
passed and every prerequisite has been physically flushed does the coordinator
write the final valid binding sector, flush and compare its exact readback.
No filesystem writer runs after activation. Any error after publication retains
and poisons the complete coordinator, any still-live child state and parent
lease until reboot; a backend error stops further I/O. A failed final write/flush may already
have persisted a valid binding, so failure status does not promise nonactivation.

The new first-party factory/planning/readback units carry Apache-2.0. The ESP
writer uses the existing reviewed FatFs implementation with its notices intact;
no external filesystem implementation or new kernel link archive was imported.

## Bound ext4 root and FAT32 EFI partition

The host integration formatter builds one GPT disk with an ext4 system partition and
a FAT32 EFI System Partition. The full staged system, `/config` guide, programs,
modules, resources and license notices remain in ext4. The ESP carries the EFI
loader, the same kernel bytes at `/system/kernel.krl`, and a 96-byte descriptor
at `/EFI/XJ380/root.bin`. The formatter leaves the source stage unchanged,
formats/checks temporary partitions with independent host tools, reads back both
kernel copies and the descriptor, then atomically replaces the requested image.
The default FAT32 build remains separate.

`XJROOT01` version one records a disk GUID, root-partition GUID, ESP GUID, ext4
filesystem UUID, kernel length and standard CRC32C, plus a descriptor CRC32C over
the first 92 bytes. GPT GUID fields use GPT on-disk byte order; the ext4 UUID uses
its raw superblock bytes. The loader accepts a descriptor only from its own
LoadedImage device, checks its hard-drive device-path GPT ESP identity, and
verifies that ESP's kernel before handing the binding to the kernel. The
BOOT_CONFIG tail is read only when the bound-root flag is present; legacy FAT
boot retains its earlier prefix contract.

The kernel requires a complete GPT inventory with valid primary header/table
IEEE CRC32 checks, non-overlapping ranges, and globally unique matching disk,
root and ESP identities. Root and ESP must be on that same physical disk and
have Linux-filesystem/EFI partition types. Labels and partition ordering are
not selection criteria. A missing, wrong, duplicate or malformed binding fails
closed: no other disk, FAT marker volume, ESP or label fallback is attempted.

Before any flush, RECOVER write or recovery checkpoint, the native mount checks
the filesystem UUID and `/system/kernel.krl` length/CRC32C. A dirty journal is
validated using its complete surviving recovery overlay rather than untrusted
home bytes. Invalid binding/kernel data therefore cannot trigger journal replay
or make a clean volume dirty. These identifiers and checksums bind one boot
image to its system volume; they are not a signature or secure-boot mechanism.

After validation, the adapter prepares a private directory tree and publishes
it in one short VFS operation. The existing `/` vnode, cwd references, `/dev`
mount and block-device vnodes keep their identities. Only unopened private
mountpoint placeholders are covered; their native cache handles and vnodes are
released after the VFS lock is dropped. Failed preparation/publication retains
the original public tree.

Normal task-context shutdown/reboot serializes ext4 operations, completes
runtime-held orphan cleanup, flushes the device and clears RECOVER before EFI
power transition. Live root/device vnodes remain allocated and reject further
backend operations. A poisoned guard or failed I/O refuses cleanup and retains
the recovery requirement. Fatal/early boot paths which cannot wait for storage
also leave recovery for the next boot.

The host formatter retains the reviewed modern ext4 features, uses 4 KiB blocks
and 256-byte inodes, and accepts an ext4 root of at most 1 TiB / 8192 block
groups. This conservative host-image profile leaves headroom within the
writer's 256 MiB arena budget; runtime admission still checks the actual inode
and group geometry. The bound applies to the root, not the total GPT disk
including its ESP and alignment space. Its journal is selected between 4 and
32 MiB, within the native 32768-logical-block journal limit. The normal public image commands are:

```sh
VDISK_FS=ext4 VDISK_IMAGE=out/XJ380-ext4.img ninja -f build.ninja vdisk
VDISK_IMAGE=out/XJ380-ext4.img ninja -f build.ninja justrun
# Equivalent direct packaging of already-built artifacts:
python3 tools/ninja_build.py vdisk --fs ext4 --image out/XJ380-ext4.img
```

`VDISK_SIZE_MIB` sets total disk size; `VDISK_ESP_MIB` sets the ESP size (at least
64 MiB). Both product and provider wrappers use the same reviewed host formatter.
Ext4 staging preserves symlinks; FAT32 staging retains its existing flattening.
The host needs standalone `sgdisk`, `mke2fs`, `e2fsck`, `debugfs`, `mkfs.vfat` and
mtools. No implementation library from these tools enters the kernel.

The binding also imposes an update boundary: replacing only the ext4 kernel or
only the ESP kernel invalidates the pair. Rebuild a complete image until a
coordinated in-place updater exists; the fresh-disk factory is destructive.
Binding checksums detect mismatch;
they do not provide an atomic in-place kernel update or authenticate the image.
Existing installations are not converted automatically.

## Remaining integration work

The overall replacement goal remains open. The trusted fresh whole-disk factory
now implements GPT/root/ESP construction, complete readback and final binding
activation. Boot staging now proves retained package memory, validates the
complete package profile and matches the actual boot source registration.
Installer packaging explicitly selects FAT32/PAK1 by default or ext4/PAK2.
An installer-intent boot publishes its verified RAM root and opens the native
versioned service only after the trusted graphics worker initializes. The
product UI uses original-caller Admin checks, opaque owner-bound handles and a
kernel-owned worker ticket. It supports fresh/full-package ext4 with a FAT32
ESP and the packaged locale. Legacy write APIs remain refused; source lifetime
validation and target exclusion apply throughout. Unsupported profiles and
failed bootstrap never fall through to an unrelated system root or shell.
The Apache provider has no product GUI and leaves admission closed.
Repair, keep-users and in-place update remain unimplemented policies; they must
identify bound volumes explicitly and preserve their intended user state.
The current fixed-path binding can fail closed across an interrupted upgrade;
it does not promise atomic old-or-new boot availability.

The VFS/procfs lifetime changes remove the earlier four expected failures while
retaining conservative refusal for real references, exhausted retirement state
and failed cleanup. Their bounded host/kernel regression evidence is separate
from whole-disk installer activation and from long-term GUI performance.

## Verification

From the parent project:

```sh
python3 OpenXJ380/tools/check_ext4_sources.py --root OpenXJ380
python3 -m unittest discover -s tests -p test_ext4_source_policy.py -v
python3 -m unittest discover -s tests -p test_native_ext4.py -v
python3 -m unittest discover -s tests -p test_native_ext4_write.py -v
python3 -m unittest discover -s tests -p test_native_ext4_format.py -v
python3 -m unittest discover -s tests -p test_native_ext4_large_format.py -v
python3 -m unittest discover -s tests -p test_native_ext4_large.py -v
python3 -m unittest discover -s tests -p test_ext4_sparse_device.py -v
python3 -m unittest discover -s tests -p test_ext4_format_adapter.py -v
python3 -m unittest discover -s tests -p test_installer_ext4_import.py -v
python3 -m unittest discover -s tests -p test_ext4_build_adapter.py -v
python3 -m unittest discover -s tests -p test_installer_disk_layout.py -v
python3 -m unittest discover -s tests -p test_installer_esp.py -v
python3 -m unittest discover -s tests -p test_installer_esp_verify.py -v
python3 -m unittest discover -s tests -p test_installer_ext4_verify.py -v
python3 -m unittest discover -s tests -p test_installer_factory.py -v
python3 -m unittest discover -s tests -p test_block_access.py -v
python3 -m unittest discover -s tests -p test_block_access_glue.py -v
python3 -m unittest discover -s tests -p test_fatfs_guard_recovery.py -v
python3 -m unittest discover -s tests -p test_vfs_mount_errors.py -v
python3 -m unittest discover -s tests -p test_vfs_legacy_unmount.py -v
python3 -m unittest discover -s tests -p test_fat32_image_geometry.py -v
python3 -m unittest discover -s tests -p test_native_ext4_mutations.py -v
python3 -m unittest discover -s tests -p test_native_ext4_namespace.py -v
python3 -m unittest discover -s tests -p test_native_ext4_mapping.py -v
python3 -m unittest discover -s tests -p test_native_ext4_attributes.py -v
python3 -m unittest discover -s tests -p test_native_ext4_xattr.py -v
python3 -m unittest discover -s tests -p test_native_ext4_xattr_census.py -v
python3 -m unittest discover -s tests -p test_native_ext4_orphan.py -v
python3 -m unittest discover -s tests -p test_native_ext4_orphan_delete.py -v
python3 -m unittest discover -s tests -p test_ext4_directory_hash.py -v
python3 -m unittest discover -s tests -p test_native_ext4_directory_index.py -v
python3 -m unittest discover -s tests -p test_ext4_cache_refresh.py -v
python3 -m unittest discover -s tests -p test_fd_lifetime.py -v
python3 -m unittest discover -s tests -p test_procfs_lifetime.py -v
python3 -m unittest discover -s tests -p test_process_exit_lifetime.py -v
python3 -m unittest discover -s tests -p test_scheduler_references.py -v
python3 -m unittest discover -s tests -p test_recoverable_mutex.py -v
python3 -m unittest discover -s tests -p test_ext4_guard_recovery.py -v
python3 -m unittest discover -s tests -p test_ext4_root_validation.py -v
python3 -m unittest discover -s tests -p test_boot_root_binding.py -v
python3 -m unittest discover -s tests -p test_hybrid_root_binding.py -v
python3 -m unittest discover -s tests -p test_provider_staging.py -v
python3 -m unittest discover -s tests -p test_bound_root_selection.py -v
python3 -m unittest discover -s tests -p test_vfs_root_graft.py -v
python3 -m unittest discover -s tests -p test_permission_create_anchors.py -v
python3 -m unittest discover -s tests -p test_ext4_write_bounds.py -v
python3 -m unittest discover -s tests -p test_native_journal.py -v
python3 -m unittest discover -s tests -p test_vfs_readonly.py -v
python3 -m unittest discover -s tests -p test_syscall_rename.py -v
python3 -m unittest discover -s tests -p test_syscall_xattr.py -v
python3 -m unittest discover -s tests -p test_syscall_metadata.py -v
python3 -m unittest discover -s tests -p test_ext4_attribute_policy.py -v
python3 -m unittest discover -s tests -p test_hpet_counter.py -v
python3 -m unittest discover -s tests -p test_ahci_flush.py -v
python3 -m unittest discover -s tests -p test_nvme_flush.py -v
python3 tools/gen_ninja.py --out build.ninja
ninja -f build.ninja all
python3 tests/run_native_ext4_qemu.py
python3 tests/run_storage_write_qemu.py
python3 tests/run_ext4_vfs_qemu.py

# Run after a completed full build; use disposable, immutable production fixtures.
python3 tests/run_bound_root_smoke_qemu.py --image out/XJ380-ext4.img --entry login
# For the provider-only CLI image, use --entry shell.
python3 tests/run_bound_root_qemu.py --base /tmp/production-fixture/base.img
python3 tests/run_bound_root_qemu.py --base /tmp/production-fixture/base.img --bus nvme --cases filesystem_uuid duplicate_disk
python3 tests/run_ext4_root_powercut_qemu.py --stage /tmp/production-fixture/root --buses ahci nvme
python3 tests/run_ext4_root_user_qemu.py --stage /tmp/production-fixture/root
python3 tests/run_native_ext4_format_qemu.py --boot-image /tmp/production-fixture/fat32.img
python3 tests/run_native_ext4_build_qemu.py --boot-image /tmp/production-fixture/fat32.img
# Trusted whole-disk factory; disposable 768 MiB targets, not the installer UI.
python3 tests/run_native_installer_window_qemu.py --boot-image /tmp/production-fixture/fat32.img --profile smoke
python3 tests/run_native_installer_window_qemu.py --boot-image /tmp/production-fixture/fat32.img --profile full --boot-smoke
```

The results below span several implementation snapshots. Historical GUI and
probe runs are not all tests of the latest binary; the fresh-build results
identify their production input hash explicitly.

Host tests use real Linux-formatted fixtures, ASan/UBSan, full payload checks and
independent e2fsck for each recovered state. They cover allocation exhaustion,
unused group initialization, stale-byte prevention, directory/link accounting,
namespace replacement/movement, and torn superblock/group/inode checkpoints.

This stage passed 528 frozen numeric hash vectors independently recorded from
separately invoked debugfs, plus a real major-hash collision pair. Indexed
directory tests passed 1,376 write/flush cut states across the four persistence
models, with leaf splits, root promotion, internal splits and collision chains
on 1/2/4 KiB blocks. Additional fixtures cover unsigned high-byte names,
checksums disabled, malformed indexes, one-way LARGEDIR upgrade, Linux journal
replay and torn checkpoints. The 4 KiB checksum-free internal split exercises
the 511-entry plus carry workspace boundary.

Large unlink/replacement and zero-link recovery passed 2,252 cut states across
the same four persistence models on a 1 KiB, twenty-allocation-group fixture,
including cuts during recovery and held middle-orphan reclamation. These counts
already include the four models. Normal tests also cover deferred unlink and
replacement, name reuse, held-inode data/metadata/xattr mutations, linked truncate
on the same orphan chain, empty directories and fast/mapped symlinks. Production
cache, FD and scheduler lifetime harnesses cover alias refresh, close/reuse races,
task cleanup and selected-task/kernel-stack reference retention.

The VFS QEMU probe passed on both AHCI and NVMe, exercising normal mutations,
cache updates, error propagation, busy unmount, read-only mounts and remount
persistence, then checking actual partition contents and metadata with Linux tools. Its separate
test kernel stops after a flushed commit before checkpoint so the runner can
SIGKILL QEMU, replay a disk copy with Linux, then boot the original disk to check
native recovery. A temporary login ELF covers ordinary POSIX/SYS calls,
including renameat, descriptor modes, fsync, open-unlink lifetime and the
application mount-permission boundary. A trusted kernel worker performs clean
unmount after the application closes its completion marker. The NVMe fixture
uses a checksummed v3 journal. This stage passed 129 kernel and 104 ordinary
user checks with zero failures, using a Linux-created indexed root with 500
fixture entries on each data disk. It also verified a child inheriting an
unlinked FD, writing/growing it, and exiting without close; waitpid and the
parent's final close preserved data and reclaimed the orphan. Independent Linux
replay, native reboot recovery, clean unmount, fsck and exact data/metadata
readback passed. This remains a disposable data-volume test, not system-root
or installer acceptance.
Host fixtures also force extent-tree depth two, triple-indirect allocation,
shared xattr copy-on-write/reclamation, pre-epoch and post-2038 nanosecond times,
Linux-created checksummed multi-transaction journals and revoke records. Large
truncate fixtures distribute twenty data blocks across twenty allocation groups
to exceed one transaction's metadata budget. They cut power during intent,
cleanup, orphan removal and recovery itself; Linux independently completes a
captured pending intent. Torn orphan-head bytes and mixed low/high inode sizes
are tested before strict final-state validation. Metadata syscall tests retain
FD nodes through close races and reject endpoint queries without closing a
live pipe or PTY. The runtime utimensat wrapper enters the kernel and propagates
errors instead of returning an unconditional success.


Bound-root acceptance evidence is recorded separately. The refusal runner boots
the production EFI/kernel with no selector replacement: bad descriptor CRC and
ESP GUID stop in EFI; wrong root GUID, filesystem UUID, root-only kernel bytes
and duplicate whole-disk GUIDs stop in the kernel. A separate readable FAT32
marker volume demonstrates that refusal does not select legacy fallback. Root
and duplicate test disks stay writable; the marker backend is read-only so OVMF
cannot add NvVars. Each protected ext4 root partition is hashed before/after the
boot, and its complete bytes must remain unchanged. The immutable source image
is also hashed and never used as a writable VM disk.

The current clock/CREATE build passed six production AHCI refusal cases and two
focused NVMe cases. Every protected root-partition hash, companion marker-disk
hash and immutable base-image hash stayed unchanged.

Actual bound-root durability tests passed separately on AHCI and NVMe. A test
kernel stops after a real device flush makes a growth transaction's commit
durable, before checkpointing it. Host SIGKILL leaves old home data and a pending
journal. Linux replays one independent copy; the native driver cold-boots the
original. Both recover the exact new bytes, sparse hole, size, uid/gid and mode,
then finish with a clean filesystem and no pending orphan. These latest runs
include the final root/user-syscall integration fixes. Injected stop hooks and
temporary test ELFs are confined to disposable test fixtures.

The clock/CREATE production hybrid GUI disk passed a 549-second AHCI OOBE/login session,
seven file-manager launches, settings changes, ordinary text creation/rewrite and
normal guest shutdown. A 501-second NVMe cold-boot/password-login session read the
same user bytes and persisted settings, then shut down normally. Both cleanups
returned zero; Linux ext4/FAT checks passed and RECOVER was clear. EFI/kernel/
descriptor readback matched the packaged copies, and the immutable base remained
unchanged. OVMF may create its own NvVars file on the ESP during runtime.

A later actual user-ELF root probe includes the final rename fixes and passed 25
checks per controller: normal account/home creation, sparse growth, shrink and
zero-filled extension, write-after-unlink and final close, rename replacement
while the previous inode is open, source preservation on refusal, and protected
home/private-IME targets. Both guests powered off normally; independent Linux
checks found the expected bytes, clean metadata, no recovery bit and no orphan
head. Its completion observer waits for the user's final marker reference to
close before starting shutdown; observing marker bytes alone is not sufficient.

The GUI build also includes stable HPET high-low-high counter reads. A host
model reproduces the old split-read rollover's false 42.95-second jump and its
ability to expire AHCI's unchanged timeout. An earlier GUI run had a real AHCI
timeout; its log lacks counter samples, so it does not establish that rollover
was that event's sole cause. Failure-only AHCI diagnostics now record elapsed
clock samples and poll counts. The successful long runs are validation evidence,
not a guarantee against all controller or hardware faults.

The activated public CLI wrappers also each produced a real 512 MiB hybrid image
(446 MiB root, 64 MiB ESP, 6 MiB journal). Independent readback verified both GPT
headers/tables, all binding identities, both kernel copies, the exact initial
three-file ESP payload, `/config` guide, root marker and Apache notices; ext4 and
FAT checks returned zero. Disposable clones of each unmodified production image
booted on both AHCI and NVMe through root publication to login/CLI process
creation. Those four short smoke runs use host quit and make no clean-shutdown
claim. Their read-only source-image hashes stayed unchanged. The separate GUI
and user-root tests above establish normal guest shutdown behavior.

The native-format phase passed eight 32–8193 MiB geometry/journal profiles,
Linux fsck before/after actual native mutations and selected backup supers,
thirteen invalid inputs with zero I/O, and 32 targeted persistence/failure
states. The isolated freestanding formatter object has no runtime imports.
Actual adapter/guard/lease tests cover unknown flush declarations, stale handles,
whole/slice aliases, independent partitions, first-error resource retention,
quiescent owner death and TCB reuse. Thread-level reaping also checks stopped
threads while their parent process remains alive; current, selected and previous
stack references prevent premature poisoning.

An earlier production snapshot's separate kernel probe formatted two initially zero
partitions on each controller without mke2fs: 129 MiB/J5 and 62 MiB/J4. AHCI and
NVMe each passed 97 checks, including mounting one partition while formatting
its sibling, busy/whole-disk refusals, ordinary VFS mutations, metadata/binary
xattrs, read-only/writeable remounts and normal guest shutdown. Independent Linux
fsck and exact content/attribute/owner/mode checks passed on all four partitions;
RECOVER/orphan were clear, GPT bytes and the immutable boot source were unchanged.
Only public EBUSY with an unchanged unpublished mountpoint was retried.

A previous no-hook GUI wave, with production kernel SHA-256 prefix `3c662b4b`,
tested both FAT32 and ext4 images: fresh account,
three file managers, settings, text creation and rewrite on AHCI, followed by
password cold-login, settings/file readback on NVMe using the same clone. All
four guest shutdowns returned zero; independent fsck and byte-for-byte user
file/settings checks passed. Ext4 sessions ran 365 and 529 seconds. Original
read-only base-image hashes and both GPT copies stayed unchanged. The FAT32
NVMe file dialog was still absent at +55.9 seconds and first observed at +74.7
seconds; later interaction/readback/shutdown recovered. This observation has
no established cause and is not a passed latency or long-term performance test.

The later fresh-build phase passed the actual formatter/importer and
adapter/guard/lease host tests, including complete preflight, resource failures,
first-error retention and quiescent owner death. Selected import failure cuts
were checked through independent native and Linux journal recovery; they do not
establish package-wide rollback. The checked VFS retirement suite passed eleven
test methods with 23 positive scenarios, and three isolated historical-ordering
sanitizer regressions demonstrate the repaired gaps. The previous four desired
behaviors now pass. Procfs passed seven actual-code sanitizer test methods plus
the FD lifetime method; its bootstrap, foreign-backend close and process/FD
retirement cases exercise the production lifetime contracts.

A separate host run imported a real staged tree with 268 records and
123,553,414 regular-file bytes into a natively formatted 512 MiB/J6 volume.
Independent Linux readback matched every file and represented metadata; the
only extra path was the empty factory `lost+found`. Linux fsck returned zero,
RECOVER/orphan were clear, and both source trees remained unchanged. That tree
contained no symlinks, hardlink aliases or xattrs; dedicated fixtures test those
features. Its payload kernel has SHA-256 prefix `67374045`, so this is a
full-tree importer test, not equivalence to the current product image, a QEMU
speed measurement or whole-disk installation acceptance.

An earlier fresh-build QEMU probe used that snapshot's final production link inputs whose unmodified
product kernel has SHA-256
`4b7b468d0ac9a27d6c2e2fbb33ac081449424c0eafed3c9c4610275cbcd0c484`.
Only the disposable probe kernel wraps root setup for its test entry. On AHCI
and NVMe it formatted and PAK2-populated two initially zero data partitions
(129 MiB/J5 and 62 MiB/J4) without mke2fs, then exercised ordinary VFS mutations,
busy/alias refusals, read-only/writable remounts and procfs observation/refusal.
Each controller passed 207 checks and powered off normally with return code
zero. Independent Linux checks passed on all four partitions, including exact
data, binary xattrs, owner/mode/nanosecond timestamps, hardlink identity and
opaque symlink text. RECOVER/orphan were clear; GPT bytes and the immutable
boot source were unchanged. This verifies fresh offline data volumes, not GPT,
ESP or root-binding activation.

A separate GUI wave used that unmodified `4b7b468d` product kernel in both public
FAT32 and ext4 image builds. Each image completed fresh AHCI account creation,
three file managers, settings changes, text creation/rewrite and menu shutdown,
then an NVMe password cold-login and GUI readback on the same clone. All four
156–259-second sessions ended with real guest shutdown and return code zero;
offline fsck, exact note/settings, GPT, kernel/EFI and ext4 binding checks passed.
Both read-only base-image hashes stayed unchanged. In each NVMe session the
first sampled Open dialog screenshot, about 2.37 seconds after the click,
already showed the dialog. This is an observation bound, not exact appearance
latency. The previous roughly 75-second delay was not reproduced; its cause
remains unestablished, and these short sessions are not long-term performance
acceptance. One FAT32 directory double-click needed a normal repeat before
entering home; subsequent GUI file readback and shutdown succeeded.

The host FAT32 verifier now accepts a bounded filesystem smaller than its GPT
partition. Actual mkfs.fat track rounding leaves three sectors unused in a
510 MiB partition and 23 in a 3070 MiB partition; these are valid volumes.
Regression images at public 512/default 3072 MiB sizes passed Linux fsck and
mcopy readback, while malformed cluster/FAT/root bounds and overrun remain refused.

### Current native whole-disk factory validation

The current actual-module host integration passed four methods and 21 scenarios
with ASan/UBSan. It executes the real coordinator, native formatter/importer/full
verifier, existing FatFs and private ESP adapter, GPT/FAT readback gates, block
registry and mutex code. Platform IRQ/TCB/VFS block backends are host seams;
synthetic ELF/MZ payloads are structural fixtures. Cases include thirteen
coordinator allocation failures, complete preflight refusals with zero I/O,
ten selected backend cuts, first-error retention and five activation persistence
models. A reused-GUID case checks that old binding bytes are cleared before new
GPT headers become visible. These are selected fault models, not exhaustive
instruction/sector tearing or hardware failure tests. The standalone full ext4
verifier also passed eight methods/41 command checks, including Linux-created
extra names/xattrs, metadata changes and a fsck-clean hardlink-identity swap.

The subsequent provider build completed 19 actions and the product build 68.
The unmodified product kernel SHA-256 is
`268e8f8f6903c2244deb11502954dff3e23de7b0f131727962548f0fa281c913`.
Using those current link inputs, a disposable probe invokes the real trusted
whole-disk API through a separate test entry on AHCI and NVMe. It loads an
immutable PAK2 from the source filesystem into retained kernel memory, excludes
that source device, and constructs an initially zero 768 MiB target without
host filesystem formatting. Both the 17-record smoke profile and the full
profile passed 20 kernel checks per controller and normal guest poweroff with
return code zero.

The full profile contains 277 records, 276 canonical inodes and 123,951,877
regular-file bytes: the current complete system stage without Linux
compatibility packages, plus explicit metadata/link diagnostics. AHCI and NVMe
runs took approximately 185.300 and 183.937 seconds. Independent Linux readback
matched the entire namespace, every data/symlink byte, mode, uid/gid, nanosecond
mtime, xattr and hardlink identity; fixtures include 59/60/61-byte and 303-byte
opaque symlink targets. Both GPT copies, ext4/FAT fsck, clear RECOVER/orphan,
binding identities and both kernel copies/EFI loader passed independent checks.
The source template and production artifacts remained unchanged. The unified
local record is `/tmp/xj380-native-installer-window-qemu-validation.json`;
host integration is recorded separately in
`/tmp/xj380-installer-factory-validation.json`.

Separate disposable clones of both full targets then cold-booted the unmodified
production EFI/kernel, without test hooks, through bound-root publication to
login process creation. Those short cold boots stop by host quit and establish
neither clean guest shutdown nor completed GUI account/login acceptance. The
`--profile smoke` runner checks the small diagnostic package; `--profile full`
checks the staged system plus diagnostics, and optional `--boot-smoke` adds
only that separate production cold-boot scope. The historical `4b7b468d` GUI wave
above remains evidence for its own binary and host-built images. Those factory
results predate the boot staging below and do not prove its source-RAM or
request boundaries. They do not complete the overall replacement goal. Shared file mappings remain
unsupported.

The same unmodified `268e8f8f` kernel also passed a separate four-session GUI
functional regression. Its ext4 base is the actual native full factory output,
not a host-formatted replacement; the FAT32 control base comes from the public
image builder. Each completed fresh AHCI account creation, settings changes,
GUI text creation/rewrite and menu shutdown, followed by NVMe password cold-login,
GUI settings/file readback and menu shutdown on the same clone. All four guest
shutdowns returned zero. Linux fsck, exact user bytes/settings, GPT, kernel/EFI
and native binding checks passed; both read-only bases and the native source
image retained their original hashes. Each session recorded fifteen ordinary
FM process/user-stack launches and independently operated three distinct FM
windows; process creation alone is not claimed as fifteen ready windows.

GUI observation is separate from performance acceptance. The ext4 NVMe Open
dialog contents were visible in the first sample at about 2.368 seconds. FAT32
NVMe showed an empty content pane at 2.366 seconds and a populated pane at the
next actual sample at 32.438 seconds, so readiness is only bounded by that
interval. Neither an exact 32-second delay nor a greater-than-ten-second stall
is established. Directory/file selection retries and FAT32 Save As input/focus
timing corrections remain in the original action/screenshot record. Final GUI
readback and normal shutdown passed; no performance root-cause or long-term
stability claim follows. The local GUI report is
`/tmp/xj380-native-factory-gui-5n5hcxrj/result.json`.

Test hooks are absent from production kernels.

See [the license review](EXT4_LICENSE_AUDIT.md) and the provider's
`driver/fs/ext4/PROVENANCE.md` / `SOURCE.json`. No external filesystem
implementation or new link archive was imported. Source hashes and SPDX checks
track reviewed inputs; they do not prove originality or replace source review.

## Licensing requirements

- The parent XJ380 project is closed source; OpenXJ380 is Apache-2.0. Any new
  linked implementation must preserve both licensing goals.
- Audit the exact source revision, every imported file, enabled build features,
  code provenance, and all transitive dependencies before importing anything.
  Record hashes, upstream URLs, SPDX identifiers, notices, and the selected
  license for dual-licensed code. A top-level permissive license is insufficient.
- No GPL implementation may enter the kernel or proprietary applications.
  Do not translate GPL code, remove its headers, or wrap it to evade its license.
  Existing explicit exceptions are not permission to import unrelated GPL code.
- Host format/check tools have a separate dependency and distribution review.
  Keep their implementations out of kernel link inputs and source bundles.
- Preserve license texts and attribution for any approved MIT/BSD/Apache code.
  Add a dependency gate to CI before enabling the replacement driver.

## Candidate evaluation

The following are evaluation candidates, not approved or imported dependencies.
Their complete source and dependency audit remains outstanding.

| Candidate | Upstream license statement | Suitability and limits |
| --- | --- | --- |
| [ext4-view-rs](https://github.com/nicholasbishop/ext4-view-rs) | Apache-2.0 OR MIT | `no_std` plus `alloc`; explicitly read-only. Useful for evaluating a read-only implementation and independent readback, not a full writable root. |
| [ext4plus](https://github.com/arihant2math/ext4plus) | Apache-2.0 OR MIT | Supports writes, but upstream reports beta bugs, lacks journal writes, and only writes xattrs within the inode body. Not ready for a persistent system root without substantial work. |
| [FreeBSD ext2fs](https://github.com/freebsd/freebsd-src/tree/main/sys/fs/ext2fs) | Must be audited per file; `ext2_extents.c` declares BSD-2-Clause | C code may fit the existing freestanding build better, but its kernel/VFS dependencies and actual feature coverage require a porting assessment. Do not assume the entire subtree has one license. |

The adopted direction is a native freestanding C++ VFS driver in OpenXJ380,
with new first-party code under Apache-2.0. Reuse only separately audited
permissive components where they reduce the porting and maintenance cost.
An entire alternate runtime should not be added solely to hide missing
filesystem functionality. Do not start a port based only on its README claims.
Use the [on-disk format documentation](https://www.kernel.org/doc/html/latest/filesystems/ext4/)
and independent Linux-generated fixtures; do not copy Linux driver code.


## Current boot staging and VFS recovery regression

The later 2026-10-31 production product kernel is SHA-256
`eb003e84beb88a3d6d39376c7521a3fb35c04a4f152bcd66da075fd508e3dadc`;
EFI is `6ee942e99ec13fa27802a0e57b9304f379fcf321df689e510bc6778d7b687e67`.
Both provider and product full builds passed. This evidence supersedes neither
the older factory/GUI tests nor their stated limits; it covers this later binary.

Actual production EFI/kernel boots passed AHCI PAK1, NVMe PAK2 and read-only
2048-byte AHCI optical PAK2 source checks. A damaged PAK2 body failed complete
validation with `-EINVAL` before source matching. All four stopped before normal
root mounting or login. They ended by host monitor quit; no native user installer
or clean guest shutdown is claimed. Optical ISO bytes remained identical.
The three writable FAT clones each changed 25 sectors in the new NvVars file
and its directory/FAT/FSInfo allocation footprint. This is consistent with
firmware variable storage, not evidence of all-stage source immutability;
no writer-time trace was collected. Read-only base and production inputs match
their recorded hashes. See `/tmp/xj380-installer-entry-qemu-czil6s83/results.json`.

A separate probe reused the exact completed build objects without regeneration;
only disposable test kernels added wrappers. Real AHCI/NVMe VFS tests passed
129 kernel assertions and 104 ordinary user-ELF assertions. Covered operations
include create/delete, growth, truncation/zero tails, replacement/cross-directory
rename, open-unlink, orphan close, metadata, xattrs and fd/path/renameat errors.
Both images passed independent Linux fsck, exact data/metadata readback and
clear RECOVER/orphan checks after clean ext4 unmount. A first guest was killed
after a durable commit before checkpointing: the home name was initially absent,
and both independent Linux replay and the next native mount recovered it.
The second guest ended by host termination after clean filesystem unmount,
not a claimed whole-GUI menu shutdown. Current source and production hashes
remained unchanged. See `/tmp/xj380-installer-entry-current-vfs-validation.json`.

Host sanitizer regressions passed five growth/truncate methods, twelve journal
methods and six namespace methods. The latter exercised 21 scenarios and 3852
write/flush cut states with independent Linux fsck checks. Boot-memory tests
passed 12100 assertions, full paging-closure tests 442 and boot-session tests
29 independent process scenarios, including an unreadable hidden-table alias.
The closure permits same-level sharing, bounds its workspace at 8192 tables
and rejects unsupported/cyclic table interpretations before reading packages.
These bounded models do not cover arbitrary hardware cache failures or torn
sectors outside their stated injection models. The complete local phase record
is `/tmp/xj380-installer-entry-validation.json`.

Installer filesystem packaging is documented in
[the package contract](INSTALLER_PACKAGES.md). User installation remains disabled
until retained source/target handles, request authorization and root publication
are integrated; no legacy writer is reopened by the new boot checks.

## Retained sources, kernel handles and private root

The later source/handle/private-root phase used product kernel SHA-256
`d85330ad05eca0b0909644deae58bb0247797aa360969cffb73cf295d01170a4`
and the unchanged production EFI
`6ee942e99ec13fa27802a0e57b9304f379fcf321df689e510bc6778d7b687e67`.
Both complete builds passed. New original Apache-2.0 code adds no external
implementation or archive. The reviewed static links have 144 product / 133
provider objects, eleven installer units and all five allocator wraps.

At that recorded revision, the source retained a generation-authenticated devfs node and an
exclusive whole-source lease owned by the boot TCB. Cross-TCB validation does
not transfer source I/O rights. Target/job handles are kernel-only, monotonic
and bound to issuing TCB/TID/PID cookies; Admin admission, source exclusion,
registration generation, whole 512-byte geometry, flush capability and current
lease conflicts are checked before queueing. Worker factory I/O remains in the
worker's own target lease. The service reports Queued/Running/final snapshots;
it never reads a concurrently written factory status. Periodic reaping handles
quiescent workers in a still-live process, and before-free hooks prevent reads
of retired TCB cookies. Ambiguous I/O/cleanup or abandoned work is quarantined.
Preparation interrupted before a retain returns may conservatively hold hidden
pins/slots until reboot; empty output does not authorize automatic reclamation.

The private root builder has no registered fsid, published vnode/root pointer,
ordinary tmpfs callback or global root/dev/cwd mutation. Its fixed profile caps
32768 inodes and a conservative 64 MiB budget, also bounded by boot-session RAM.
Every ordinary allocation-failure prefix rolls back; unexpected references
retain the tree instead of unsafe cleanup. Sealing is not mounting or Ready.
The legacy raw-pointer prepare path is refused; launch no longer fabricates a
Root user or substitutes a shell. A safe managed backend and atomic publisher
must be added before executing this private tree; ordinary tmpfs callbacks were
not rewritten or used by this staging path.

Host ASan/UBSan checks passed 34 source-lifetime scenarios, 42 job scenarios,
31 boot-session processes and private-tree tests covering 30 OOM prefixes,
200 concurrent snapshot/build/discard rounds and quarantined lifetimes.
Job tests link the actual registry/admission implementation but use an external
factory-result adapter; they are not a user-to-worker real-disk installation.
Existing registry, devfs/FAT glue, FD/process cleanup and legacy refusal tests
also passed.

Two production EFI/kernel fixture groups each passed five boots: AHCI PAK1,
NVMe PAK2, read-only 2048-byte optical PAK2, damaged system PAK2, and a root
missing the installer executable. The complete root came from the actual
installer-root staging command: 18,200,013 PAK1 bytes, 94 wire records plus the
implicit root, and 18,195,588 copied payload bytes. All three valid transports
sealed 95 nodes with published=0; the actual job admission returned `-ENODEV`,
Ready=0 and handle=0. The damaged system package was rejected before retaining
the source; the missing executable rolled the private tree back to zero entries
and bytes. All stopped before system-root fallback or login and ended by host
monitor quit. Read-only ISO bytes were unchanged. Writable source clones each
changed 25 sectors in a new NvVars file's allocation footprint; existing root
entries stayed identical. This is a footprint comparison, not a writer-time
attribution or zero-source-write assertion.

Current objects also passed a separate ordinary AHCI/NVMe VFS regression:
129 kernel and 104 user-ELF assertions, independent Linux fsck and exact
readback, clean ext4 unmount/RECOVER/orphan checks, and actual AHCI SIGKILL after
a durable commit before checkpointing with Linux and native replay. Test hooks
exist only in disposable probe kernels; production hashes stayed unchanged.
This is not a current GUI menu-shutdown or native job-factory acceptance test.

The complete phase record is `/tmp/xj380-installer-handles-validation.json`.
At that revision user installation remained unavailable. The subsequent phase
below adds the managed root, versioned request adapter and worker dispatch. Repair, keep-users, partial-component
and post-activation locale mutation paths remain unsupported.

## Managed installer root and native user requests

The next phase adds an original managed RAM backend, a bounded deep-tree
publisher, native ABI version 1, mirrored user SDK and a fresh-install GUI.
Publication retains the existing root vnode address, cwd references and `/dev`
source identity. Backend ownership pins keep RAM files from ordinary cache
eviction. Empty `/pipe`, `/sock` and `/proc` mountpoints receive an explicit
boot-owner handoff; ordinary overmount of RAM-owned nodes returns `EBUSY`.
`/tmp` remains in the managed RAM root. No network modules or normal login,
desktop or shell fallback run in installer mode.

The backend supports read/stat/create/write/resize/delete and no-overwrite
cross-directory rename. Unsupported RAM links, mmap and xattrs return
`EOPNOTSUPP`; these limits do not remove ext4's existing features. The 64 MiB /
32768-node bootstrap budget remains. Resize can temporarily retain both old and
new buffers, and large copies hold the entry lock; this is a bounded installer
RAM backend rather than a general-purpose tmpfs performance claim.

Service requests copy fixed-width versioned structures using the original
process page directory, checking effective page-table USER/WRITE permission.
No user pointer reaches a worker. Target handles are opaque and owner-bound;
selection and submit require Admin, while the issuing owner may cancel queued
work or release resources after a privilege downgrade. New GUIDs/UUIDs use
checked hardware entropy; unsupported/failed entropy returns before target I/O.
Worker publication prepares TCB/stack/queue nodes before locking and safely
allows a worker to exit immediately. The legacy thread API remains separate.

The UI runs all handle operations on one main TCB; window callbacks enqueue
bounded frame-tagged actions. Fresh, complete ext4 installation and the packaged
locale are the only accepted write profile. Running work cannot be silently
cancelled or freed. An uncertain/quarantined result never offers safe retry or
success shutdown. The shutdown button requires the queried durable-success
state with zero error and no unknown/I/O-failed flag.

Host checks cover 39 actual service scenarios, 8 actual SDK/UI scenarios,
45 runtime/admission scenarios, managed-root OOM/concurrent mutation and mount
handoff, and 200 immediate-worker-exit publications under ASan/UBSan. Both full
builds pass, with 146 product / 135 provider kernel objects, twelve installer
units, the existing sole `liballoc-x86_64.a` archive and all five allocator wraps.
These host checks do not substitute for real user-to-worker disk acceptance.
Use `tests/run_installer_native_qemu.py` with current complete root PAK1/system
PAK2 inputs for a production EFI/kernel GUI installation on disposable media.
Repair, keep-users, partial-component and post-activation locale changes remain
unsupported. This work imports no external implementation or new link archive.

Production GUI/native acceptance used kernel SHA-256
`6f832c33e05a15f4abd7024cd014802e84aaf55399f9239554065b6e91b072fe`
and the unchanged production EFI. The staged PAK1 root is 20,398,078 bytes
and publishes 97 nodes. The complete native system PAK2 is 125,167,723 bytes,
268 records and 125,144,042 regular-file payload bytes; Linux compatibility
packages and test-only metadata additions are excluded from this fixture.
Real user confirmation on initially zero 768 MiB AHCI and NVMe targets dispatched
the actual kernel worker, reached durable queried GUI success and shut down via
the UI. QMP recorded guest-shutdown and process exit zero for both, with no host
quit. Independent Linux checks matched the entire namespace, contents, mode,
owner, timestamps and represented links/xattrs, GPT CRCs, FAT32 ESP and exact
kernel/EFI/binding. `e2fsck -fn` and `fsck.fat -n` returned zero, RECOVER/orphan
were clear, and all eight generated identifiers were distinct across the cases.
Read-only El Torito source ISO hashes stayed unchanged. No probe kernel, Ready
override, host ext4 formatter or host mount participated. Runs took about
762.074 / 572.431 seconds under TCG; this is bounded functional evidence, not a
storage performance or arbitrary power-loss guarantee.

The real bootstrap exposed a missing-path errno assumption in mountpoint
handoff. The fix uses idempotent mkdir before open; its regression now compiles
the actual VFS resolver and also reproduces the old failure. The frontend runner
accepts a nonnegative launch PID rather than requiring zero. The AHCI VM was
continued through the corrected frontend without changing kernel/session state;
its independent case evidence and the subsequent NVMe case both pass.
The record is
`/tmp/xj380-native-installer-production-lt2vwkvt/acceptance-frozen/results.json`.

A clone of the actual AHCI installation then booted with no file upgrades.
The first AHCI boot created the fixture account through the real first-run UI;
the same clone on NVMe showed the existing-account password page, logged in and
launched 15 file-manager processes/user stacks plus Control Center. Both boots
returned to the desktop and shut down through the UI with guest-shutdown/exit
zero. After the second shutdown, Linux `e2fsck -fn` returned zero, superblock
state was clean, RECOVER/orphan were zero and the account registry plus profile
files remained. The root and ESP kernel hashes match `6f832c33...`; the installed
graphics module matches the tested production artifact. The original factory
output remains unchanged. Evidence, nine screenshots and offline checks are in
`/tmp/xj380-installed-native-gui-regression-juqki2f1/result.json`.
The combined phase record is `/tmp/xj380-installer-native-validation.json`.

## Large-volume arenas and validation

The 2026-10-31 large-volume change replaces the writer's fixed group arrays
with a checked explicit arena while retaining the small-volume inline path.
Exact mapping, external-xattr and held-orphan bitmaps use independently tracked
4 KiB page generations, so a new transaction does not clear the entire volume's
ownership bitmap. Sorted metadata ranges cover group bitmaps, inode tables and
superblock/GDT/reserved-GDT regions. A primary-GDT read cache is invalidated
before every journal write request, including a request that fails. Recovery's
global external-xattr owner census scans all allocated inodes in batches of
1024 distinct EA blocks; this is a batch size, not a filesystem-wide EA limit.
The existing per-EA-block reference-count limit remains 1024.

The frozen native formatter report is
`/tmp/xj380-large-formatter-validation.json`, with its passing run in
`/tmp/xj380-large-format-new.log`. The actual formatter produced a
32 GiB plus 4 KiB volume with 257 groups, including a final one-block group.
Linux `e2fsck -fn` returned zero using both the primary superblock and the
backup at group 243; native parsing and independent directory/journal readback
also passed. Every native format write callback ran. The fresh sparse test
device elided zero-only writes into known-zero holes, avoiding physical storage
of roughly 514 MiB of initialized inode tables. That optimization is only a
test model for a newly truncated zero target, not an overwrite-format shortcut.
Pure planning also passed at 1 TiB and 0xffffffff blocks; those cases do not
claim completed native formatting at either size. Native formatting at 1 TiB
would still initialize 16 GiB of inode tables in this 8192-inodes/group profile.

All six methods in `tests/test_native_ext4_large.py` passed, recorded in
`/tmp/xj380-large-volume-host-validation-a5.json` and
`/tmp/xj380-large-volume-frozen-six-a5.log`. The external arena began filled
with 0xa5, checking that callers need not zero it. ASan/UBSan tests used immutable sparse
Linux fixtures and bounded durable-page overlays. They covered a 32 GiB volume with inode and
data allocation in group 128, cross-group truncate/reclamation, and rejection
of a high-group metadata alias and invalid arena before writes. All 118 selected
write/flush positions across four persistence models produced 472 native
recoveries; four pending-orphan snapshots also passed independent Linux replay.

A 1 TiB / 8192-group Linux-formatted sparse fixture passed native creation,
8209-byte write/readback, a 600-byte external xattr, rename, clean close and
independent Linux fsck. It deliberately has low inode density. A separate
1 TiB lazy-inode-table fixture with 8,388,608 inodes verified held unlink,
write-after-unlink, final reclaim and cold recovery for inode 4194305 in group
4096, beyond the former held-orphan bitmap limit. These are small amounts of
I/O on large logical geometries, not a filled 1 TiB disk, a full native 1 TiB
format, a high physical-block-above-2^32 test, or a performance measurement.

`/tmp/xj380-large-writer-xattr-census.log` records a separate passing recovery
test with 1100 distinct external EA blocks. It checks exact values after a torn
checkpoint and refuses a checksummed but incorrect reference count on an
unmodified inode's EA block in the second batch before writes or flushes.
`/tmp/xj380-large-sparse-oracle.log` records the sparse test device's persistence,
source-isolation, bounded-copy and large-byte-offset checks. Host-image boundary
tests in `/tmp/xj380-large-format-hostimage.log` also reject a root above 1 TiB
before invoking the formatter. These host fault models do not establish real
hardware power-loss behavior or arbitrary torn sectors. Earlier VM results above
retain their own binary and size scope.

Both OpenXJ380 and the closed parent passed the canonical `ninja all` target
with the new source inventory. The parent production kernel SHA-256 is
`2bf3eb45b1bf79693dd31825fdaad1f7a894b9719710a5a095397d4b8946f3b4`.
`tests/run_ext4_large_qemu.py` linked a disposable probe into those completed
kernel objects and exercised two separate 32 GiB / 256-group clones through
actual AHCI and NVMe VFS callbacks. All 44 checks passed, including inode/data
allocation in group 128, writes, external xattr, growth zeroes, truncate,
rename, retained-file unlink and exact readback, and clean unmount. QMP reported
`guest-shutdown` and QEMU exited zero without host quit. Independent Linux
`e2fsck -fn` returned zero for both disks; file bytes, 600-byte xattr, mode,
uid/gid and removed names matched, and `debugfs bmap` confirmed group 128 data.
The immutable source and completed production outputs were unchanged. Evidence
is `/tmp/xj380-large-ext4-qemu-v7q2j1df/results.json`. This is a probe-kernel
32 GiB data-volume check, not a 1 TiB guest format or a performance benchmark.

The updated existing namespace suite passed all six methods, including 3852
write/flush cut states across 1/2/4 KiB volumes. The full xattr suite passed all
eight methods and 3272 such states. The earlier xattr harness expected a hidden
recovery allocation that no longer exists; its corrected allocator-refusal case
now verifies successful recovery without that allocation, and the entire suite
was rerun. `/tmp/xj380-large-writer-validation.json` preserves both the original
stale-harness failure and the successful superseding run.

A separate regression used the current unmodified production kernel in an
upgraded disposable clone of an existing native ext4 installation. The clone's
root and ESP kernels and boot binding were updated together before boot; its
source image was untouched. The same clone booted on NVMe and then AHCI, reached
its existing-user login and desktop, and shut down using the visible power menu
on both buses (guest-shutdown, exit zero, no host quit). NVMe opened fifteen
file managers and Control Center; AHCI opened two file managers. Independent
post-GUI Linux ext4 and FAT checks returned zero, RECOVER and orphan head were
zero, state was clean, and both kernel copies/binding matched. This is an
existing-system GUI regression rather than a new whole-disk factory install.
Evidence is `/tmp/xj380-large-production-gui-1joz04kj/results.json`.
The combined current phase record is `/tmp/xj380-ext4-large-validation.json`.

## Stages and acceptance criteria

1. **Dependency and design review.** Pin any approved inputs and their complete
   dependency closure. Document a feature matrix, block I/O contract, cache and
   inode lifetimes, permission checks, and locking. Keep allocation and I/O out
   of spin-locked or scheduler-disabled regions. Preserve allocator wrappers.
2. **Read-only filesystem.** Implement bounded superblock/group/inode parsing,
   checksums, extent-tree traversal, directories, symlinks, sparse and unwritten
   reads, and metadata. Support tested 1/2/4 KiB blocks. Reject unknown features
   and volumes needing recovery; never replay a journal on a read-only mount.
   Verify corrupt images fail without writes, hangs, or out-of-bounds access.
3. **Experimental writes.** Implement allocation, create/delete, rename, links,
   growth/truncation, metadata persistence, cache invalidation, and busy-unmount
   checks. Restrict writes to explicitly supported journaled disposable images
   and validate allocation/namespace after-states. Do not enable a writable
   system root at this stage.
4. **Journal and durability.** Implement and validate JBD2 replay and transaction
   writes, checksums, revoke records, ordering, and device flush barriers. Refuse
   unsupported journal variants. Latch I/O failures and stop further mutation.
   Verify crash points with Linux fsck and byte-for-byte data checks before
   supporting writable journaled roots.
5. **xattr and application interface.** Connect get/set/list/remove operations
   through VFS and path/fd/no-follow syscall variants. Test binary/empty values,
   size queries, CREATE/REPLACE flags, hard-link identity, inode-body and external
   attribute storage, remount persistence, and namespace permission boundaries.
   Do not treat stored ACL/capability attributes as implemented enforcement.
   Unsupported operations must return an explicit error rather than fake an
   empty attribute list. Persist chmod/chown through filesystem callbacks too.
6. **Boot and packaging.** Re-enable the ext4 root plus FAT32 ESP option only
   after the feature matrix is tested. Select formatting features from that
   matrix; retain FAT32-only builds. Verify `/config`, modules, app installation,
   GUI login, multiple application launches, fsync, clean unmount, remount, and
   power-interruption recovery. Keep installer format selection explicit.

Every stage requires host sanitizers and malformed-image tests where applicable,
Linux `e2fsck`/`debugfs` cross-checks, and QEMU verification of the VFS/syscall
integration. Read-only, experimental-write, and persistent-root readiness must
be reported separately. Passing a build is not evidence of disk correctness.
