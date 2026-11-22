# Installer package formats and validation

The package validator and encoder are original Apache-2.0 implementation. They
use no third-party archive implementation or new link archive. The shared
freestanding API is `include/installer/package.h`, implemented by
`kernel/installer/package.cpp` in OpenXJ380. The parent mirrors the public header.

The existing two-argument `python3 tools/make_pak.py STAGE OUTPUT` command still
emits PAK1. Its supported entry path limit is now 511 bytes, matching both
existing consumers. PAK1 keeps its historical omission of symbolic links, but
the encoder prints an explicit count when that happens. PAK1 does not preserve
hardlink identity, ownership, timestamps or xattrs; duplicate regular-file names
remain independent data. It omits the implicit root entry as before. Unsupported
host inode types and invalid/overlong paths fail before output publication.
The current FAT32 copy profile limits a regular file to `0xffffffff` bytes;
both its encoder and full consumer precheck reject exactly 4 GiB before reading
the host payload or mutating the target. The generic PAK2 profile is separate.

`python3 tools/make_pak.py --format pak2 STAGE OUTPUT` explicitly selects PAK2.
It preserves regular files, directories, opaque symlink bytes, direct hardlink
identity, mode/owner/mtime and permitted extended attributes. This option does
**not** enable PAK2 extraction in the legacy installer. Existing extraction and
repair paths validate PAK1 completely and explicitly reject PAK2. The separate
trusted `installer_build_ext4_disk` kernel factory consumes PAK2 through
the fresh-volume importer and native installer service described below.
Metadata represented in a package is distinct from metadata actually
applied by the old PAK1 installer.

The staging tree must be frozen. The encoder rejects output within that tree,
uses `lstat` and no-follow file opens, validates before writing, then writes and
fsyncs a separate temporary file before replacing the destination. Failure
before replacement preserves the old output and removes the temporary file.
This is not a race-free snapshot of a concurrently changing directory hierarchy
or a promise of directory-entry durability after a host crash. Existing `.git`
and `__pycache__` directory exclusions are retained. The collector has bounded
entry/byte limits but can hold several host-memory copies; these are not measured
peak-RSS bounds. The CRC32C implementations use independent host/kernel lookup
loops. CRC detects corruption and provides no package authentication.

## PAK2 wire format

All integers are little endian, except no integers are embedded in paths or
symlink target bytes. There is no alignment padding, compression, or packed C
struct dereference. CRC32C uses polynomial `0x82f63b78`, initial `0xffffffff`,
final XOR `0xffffffff`. CRC detects damage, not malicious modification.

The 64-byte package header is:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | bytes[8] | `XJPAK20\0` |
| 8 | u32 | version 2 |
| 12 | u32 | header bytes 64 |
| 16 | u64 | exact total package bytes; no trailing bytes |
| 24 | u64 | record count |
| 32 | u64 | canonical inode count, including root; aliases do not add inodes |
| 40 | u64 | sum of canonical regular-file data bytes |
| 48 | u32 | flags, currently zero |
| 52 | u32 | reserved, zero |
| 56 | u32 | CRC32C of every byte after the 64-byte header |
| 60 | u32 | CRC32C of the first 60 header bytes |

Each record starts with a 64-byte fixed header:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u64 | exact record bytes including fixed header and variable fields |
| 8 | u32 | path byte count |
| 12 | u16 | REGULAR=1, DIRECTORY=2, SYMLINK=3, HARDLINK=4 |
| 14 | u16 | xattr count |
| 16 | u64 | canonical object identity, described below |
| 24 | u64 | data/target byte count |
| 32 | u32 | permission/special mode bits only, mask 07777 |
| 36 | u32 | uid |
| 40 | u32 | gid |
| 44 | u32 | mtime nanoseconds, 0 through 999999999 |
| 48 | i64 | mtime seconds, separately encoded without loss of nanoseconds |
| 56 | u32 | complete xattr TLV byte count |
| 60 | u32 | reserved, zero |

The variable fields are path bytes, data/target bytes, then xattr TLVs, without
terminators or padding. One xattr TLV is u16 name bytes, u16 flags=0, u32 value
bytes, raw name, raw value. Xattrs are strictly ordered by complete raw name;
duplicate names, embedded NUL names and unknown TLV flags are errors.

Path names are exact raw bytes, not Unicode-normalized or case-folded. The path
must be absolute; `/` is the first record and a DIRECTORY. All other paths are
strictly increasing in unsigned-byte lexicographic order, with no NUL, empty
component, repeated slash, trailing slash, `.` or `..`. Every component is at
most 255 bytes. Every ancestor must have an explicit DIRECTORY record. A file,
symlink or hardlink alias can never serve as an entry parent. This also rejects
duplicate and contradictory entries before any target operation.

For every non-HARDLINK record, object identity equals its 1-based record
ordinal. A HARDLINK record directly references the ordinal of a canonical
REGULAR or SYMLINK record. Forward references are valid. Alias-to-alias links,
self aliases, cycles, directory aliases and out-of-range identities are errors.
Aliases carry zero mode/owner/time/xattrs/data fields: all inode metadata belongs
to the canonical record and must be applied exactly once. Different paths do
not become hardlinks merely because their data is equal.

SYMLINK data is an opaque, nonempty, NUL-free byte string within the target
length limit. Absolute targets, `..`, non-UTF8 bytes, dangling targets, self
targets and cyclic links are preserved. Validation and import must not resolve,
canonicalize or follow these targets. Target resolution diagnostics, if ever
added, must be a separate non-default tool. Safety comes from explicit DIR
parents and no-follow inode-relative creation in an exclusive destination.

## Current bounded import profile

Limits are supplied by the trusted caller. The shared parser defaults are 8 GiB package/data,
4 GiB per regular file, 262144 records and canonical inodes, 4095 path/target
bytes, depth 128, and 32 xattrs per inode. These are format/profile limits, not a
claim that an 8 GiB resident package fits a real kernel's memory budget. The
caller must lower them to actual staging memory, free inodes and storage.

The arena is caller-owned, non-overlapping storage for 112-byte Records on the
x86_64 host tested here; it has no kernel allocation dependency. A 262144-record
arena requires approximately 28 MiB. All pointers are borrowed. The caller must
pin immutable, mapped, non-user-writable package memory and arena until apply
completes; copying a Plan does not establish those lifetimes or a device lease.

The timestamp representation is signed seconds plus a separate unsigned nsec
field. The current native ext4 target profile accepts seconds from
`-0x80000000` through `0x37fffffff` and the full subsecond range. Tests include
pre-epoch, both boundaries and year 2300. There is no signed-64 total-nanoseconds
conversion or accidental 2262 cutoff. The target timestamp profile assumes fresh 256-byte
native inodes with sufficient extra timestamp fields; applying to legacy small
inodes would require an additional target capability check before any write.

Default xattr imports allow only nonempty `user.*` keys. `trusted.*` and
`security.*` require separate explicit caller opt-ins; this grants permission
to store bytes, not ACL/capability enforcement. All `system.*`, POSIX ACL keys,
unknown namespaces and unsupported policy names are rejected, never silently
dropped. In particular this component does not implement ACL checking or
capability semantics. Security-xattr opt-in must not be exposed to untrusted
package authors as a privilege bypass.

The conservative ext4 storage cost for all attributes together is bounded to
3968 bytes using `16 + align4(full_name_bytes) + align4(value_bytes)` per entry.
Using the full namespace prefix overestimates native ext4's suffix storage and
fits a fresh single external 4 KiB xattr block. The exact target's support still
needs a preflight capability check; this is a restricted import profile, not
support for arbitrarily large xattrs or every existing Linux xattr arrangement.

## Legacy PAK1 reader

The original `XJPAK10\0` reader remains supported alongside PAK2. The encoder
retains PAK1 as its default, with PAK2 available only through the explicit option.
The reader uses explicit byte loads for the original 16-byte header/entries,
validates the complete buffer, rejects malformed/trailing data, then sorts
records with an allocation-free heap sort in the caller arena. All explicit
entry parents must be directories; an implicit logical `/` is accepted for
well-formed historical output, which omitted root. Object identities remain
the original record ordinals; PAK1 has no hardlink identity information.

PAK1 records expose only `Metadata::Mode`. UID, GID and mtime are absent, not
promoted to valid zero metadata. A future importer must select documented
defaults or reject a policy requiring unavailable fields. The validated reader
intentionally rejects malicious paths that the old consumers accepted. It does
not reproduce those unsafe semantics, infer hardlinks, or recreate symlinks
that the old encoder omitted.

## Shared API and caller lifetimes

`inspect_header(package, bytes, limits, info)` checks mapped-span arithmetic,
format/header fields, record count, minimum possible record bytes, allocation
multiplication and declared PAK2 budgets. It checks the PAK2 header CRC. It does
not check the body CRC or records and never returns a valid Plan. A caller may
use its `record_count` to allocate a bounded `Record` arena, but must not perform
any target operation until `validate` succeeds.

`validate(package, bytes, limits, workspace, plan)` checks the entire body,
including PAK2 CRC and all record/path/parent/identity/metadata relationships.
The arena must have sufficient capacity and valid alignment; wrapped ranges,
NULL storage and overlap with the immutable input are rejected before the first
arena write. PAK1 is sorted in place in caller storage; no parser allocation is
performed. On every failure the Plan is cleared and invalid, although the arena
may have changed. No target filesystem/device callback exists in the API.

The trusted caller must pin a mapped, immutable, non-user-writable package and
its arena until all Plan uses finish. Neither pointer arithmetic validation nor
Plan validation establishes that arbitrary physical or user memory is mapped.
Copying a Plan does not extend lifetimes, take a disk lease, reserve storage,
establish a pinned destination root, prevent concurrent namespace replacement,
or authorize privileged xattrs. Caller-specific limits must fit real resident
memory, free inodes, available disk blocks and the target inode feature profile.

The existing legacy helper first calls `inspect_header`, then full `validate`,
and frees its temporary arena after checking. Existing byte walkers consume the
same retained immutable package. They do not acquire richer PAK2 semantics by
sharing the validator. A malformed late record must fail before any earlier
record is extracted or selected for repair copying.

## Native fresh-volume PAK2 import

The original `kernel/installer/ext4_import.cpp` now implements preparation,
conservative block/inode budgeting and inode-relative application through the
native WriteSession. Preparation validates the complete PAK2 and builds ordinal
maps without I/O. Only the native formatter's fresh 4 KiB/256-byte-inode profile
is accepted. Before the writer's first flush/write, target checks validate the
factory feature set, empty root/lost+found, initialized inode fields, bitmaps and
free counts. The WriteSession's initial validator repeats that check. This is
a fresh-volume importer, not an existing-target update or general archive tool.

It creates restrictive directories/files, writes exact canonical contents,
creates opaque symlinks and direct alias names, applies canonical ownership
before final mode and user xattrs, then finishes mtimes in reverse path order
with root last. Atime is preserved. Forward references to canonical files or
symlinks keep inode identity; link text is never traversed. A package directory
`/lost+found` reuses factory inode 11; absent metadata leaves that directory
intact. Unsupported namespaces, symlink user xattrs and known link-count limits
are refused during preparation. ACL/capability enforcement is not implemented.

The trusted `ext4fs_build_node` API validates package and exact factory geometry
budgets before formatting, then retains the same physical extent lease through
format and import. It allocates and publishes every session/map/workspace and
device pin before the first backend request. The caller must retain immutable
kernel package storage; this is not a user-pointer interface. A failed backend
stops all further I/O. Any error after writer start quarantines the complete
lifetime and physical range until reboot, without retry, close or clearing
RECOVER. Successful completion alone closes the writer and releases resources.

The related `ext4fs_build_window` API borrows a same-TCB
`BlockAccessWindow` from an exclusive real whole-device lease. Every callback
reauthenticates the lease/TCB/TID, registration generations, independently
resolved node and real 512-byte logical-sector geometry, bounds the partition
request and translates it to the whole node. Flush targets the physical whole
device. The child never releases the parent token; ordinary overlapping claims
still return EBUSY, including those by the same owner. The caller retains its
node, lease and immutable mapped package through success or reboot after
quarantine. This interface does not create a user mapping or asynchronous borrow.

Budgeting includes conservative directory/mapping/xattr overhead and spare
blocks. It is not a physical-space reservation. A partial committed tree can
remain after interruption. Reported data bytes count confirmed prefixes; an
ambiguous failed request can persist more. The package is not atomic as a whole.

## Trusted fresh whole-disk PAK2 factory

`installer_build_ext4_disk` now coordinates a destructive fresh GPT disk with an
ext4 root and minimal FAT32 ESP under one exclusive whole-target lease. Its
trusted caller supplies real retained source/target nodes, distinct disk/root/
ESP identities, a filesystem UUID and mapped immutable kernel package storage.
The source physical device is excluded. The actual source and caller's request
must be authenticated by a future installation entry; an arbitrary supplied
source node is not proof of authorization. The profile requires real 512-byte
logical sectors; 4Kn is refused before I/O.

Complete package validation, required kernel/EFI/config payload checks, GPT
planning and root/ESP geometry and budgets precede target I/O. The shared pure
`esp_plan` checks FAT geometry/payload cost; subsystem readiness queries refuse
known unavailable or recursively owned state without acquiring guards or
reserving availability. Each coordinator/child layer publishes its complete
heap workspace and device pins before its own first request. Backend failure
stops further I/O; an error after publication retains and poisons the complete
lifetime and parent lease until reboot, without partial cleanup or retry.

The new first-party factory/planning/readback units carry Apache-2.0. The ESP
writer retains the existing reviewed FatFs implementation and notices; no new
external filesystem/archive implementation or kernel link archive was imported.

The factory first withdraws old MBR/GPT discovery, flushes and verifies it, then
clears the complete new ESP and flushes before publishing new GPT headers.
This prevents GUID reuse from exposing an old valid binding during construction.
It builds the root from the complete package and a private ESP containing the
EFI loader, the same kernel bytes and a zero-filled 96-byte binding placeholder.

The independent read-only ext4 verifier walks the entire manifest by inode
without following link text. It requires exact names, contents, canonical inode
and hardlink identities, uid/gid/mode/nanosecond mtime and `user.*` xattrs,
including root metadata. Only an omitted empty factory `lost+found` inode 11 is
allowed as an extra. UUID, clean journal/RECOVER/orphan state and native factory
allocation metadata are also checked. This logical manifest/allocation census
does not constitute a general data-block ownership fsck or media authentication.
A separate read-only FAT32 parser checks exact ESP files, complete disjoint
chains, mirrored FATs and BPB/FSInfo/backup metadata, and proposes a publication
sector without writing it.

Only after checked writer closure, complete root/ESP readback and a real
prerequisite flush does the coordinator write the valid binding sector, flush
and compare exact readback. The `XJROOT01` descriptor remains 96 bytes. A failed
final write/flush may already have persisted a valid binding; error status does
not promise nonactivation. Destructive fresh construction provides no old-or-new
upgrade availability or package-wide rollback.

## Partition-aware product installation

The native GUI enumerates GPT partitions and MiB-aligned unallocated extents,
using `Disk 0 Partition 1` / `硬盘 0 分区 1` names. Disk numbers are the current
enumeration order; partition numbers remain the actual GPT entry slots.
New and Delete edit only the selected GPT entry. New creates an unformatted
partition; Format independently offers ext4 (default), FAT32, exFAT and NTFS.
These are quick formats, not secure erasure. The installation root stays ext4,
and an ESP stays FAT32. The writable profile is GPT, 512-byte logical sectors,
128 entries of 128 bytes or fewer entries with the same entry size. MBR primary
partitions are display-only; extended/logical editing, conversion, resizing and
moving existing filesystem contents are outside this profile. Unsupported or
damaged disks remain unavailable without hiding other usable disks.

A single protective MBR may use either the exact disk-sector count minus one
or a saturated `0xffffffff` size, including disks below 2 TiB. Its start LBA
must still be one, and hybrid MBRs remain unsupported. This compatibility does
not bypass either GPT header CRC, backup-array comparison, partition-array CRC,
identity, placement, range or overlap checks. Existing MBR bytes remain intact
during GPT edits. A successful compatibility scan logs `partition compat` with
`pmbr-size=0xffffffff` and `GPT-verified`. Rejections log `partition reject`
with a specific reason, followed by the metadata LBA/slot and actual/expected
values; for example, `reason=backup-header-crc` identifies a failed backup GPT
header rather than a generic `-22`.

The review page identifies the selected partition and requires Apply/F8 before
any queued change. F3 opens New, Delete opens Delete, F6 opens Format (and cycles
formats on its review page), and F7 cycles eligible ESPs. Enter reviews an
installation. New/unallocated-region size is in MiB; typing replaces the initial
full-size value. Editing the partition list requires a fresh kernel-issued disk
handle and a complete partition-table fingerprint, rechecked by the worker.
GPT commit preserves unrelated entries/header extensions, writes the backup
first, flushes/readbacks, then publishes and verifies the primary. This ordering
does not provide transactional recovery from power loss. A failed/short backend
request stops further I/O and quarantines its lifetime to reboot.

An eligible FAT32 ESP on the selected disk is preferred. Other disks' ESPs are
used only after the user explicitly chooses one; the review page identifies the
cross-disk boot dependency. With reuse, the entire selected partition becomes
the root. Without reuse, 100 MiB at the beginning of the selected partition or
unallocated extent becomes a new ESP and the remainder becomes the root. No
neighboring partition is moved or resized. Existing ESP free capacity is checked
before target mutation; corrupt/unusable or insufficient ESPs cause refusal.
This precheck is not a complete filesystem repair/crosslink audit.

Reuse mounts the existing ESP without formatting it. Only
`/EFI/XJ380/BOOTX64.EFI`, `/EFI/XJ380/kernel.krl` and `/EFI/XJ380/root.bin` are
created/replaced. Other vendors' files and `/EFI/BOOT/BOOTX64.EFI` are preserved.
A fresh ESP also receives the fallback `/EFI/BOOT/BOOTX64.EFI`. The root and our
boot files are read back before the valid 96-byte root binding is published.
UEFI registration then creates/reuses an exact XJ380 Boot#### option and prepends
it to BootOrder while preserving every other entry. Firmware-variable failure
is reported separately; files may already be durable, and installation does not
claim success in this case. Secure Boot signing and arbitrary firmware NVRAM
implementations are not covered by the host checks.

The root package is still streamed PAK2 with complete metadata and link handling.
Its staging root excludes `/EFI` and `/system/kernel.krl`. The separate
`out/installer-boot/EFI/XJ380/` tree holds the loader and kernel as ordinary files;
the ISO puts that tree under `/installer/boot-files/`. Streaming-index version 3
contains the main PAK2 metadata, a small boot-file catalog and logical ranges
bound to fixed file IDs. Native validation/CRC checks cover both streams before
the first target write. Versions 1 and 2 and the legacy five-argument ISO script
remain supported for older factory fixtures. Copying version 3 files to another
FAT32 carrier preserves the index; changing package contents requires rebuilding
it. Versions 1/2 still require regenerating their physical-extent index after
repacking the FAT filesystem.

After verifying a vendor ESP kernel's size/CRC against the binding, the loader
sets `BOOT_FLAG_ESP_KERNEL_VERIFIED` in the existing boot-flags prefix. This
trusted boot-only signal selects the ext4 validator that checks root UUID and
`/system` structure before replay, without requiring a duplicate kernel on the
root partition. Legacy boots keep the original root-kernel content comparison.
Neither the boot-configuration prefix layout nor the 96-byte binding changes.

The original NTFS 3.1 formatter creates bounded, empty 512-byte-sector volumes
with 4-KiB clusters and 1-KiB MFT records (32 MiB through approximately 16 TiB).
It adds no linked NTFS library. Source provenance and host-oracle limitations
are recorded in `driver/fs/ntfs/PROVENANCE.md` in the provider. Host NTFS utility
read/write acceptance does not replace Windows/chkdsk acceptance.

The product native user installer supports a fresh full-package ext4 root
with a separate FAT32 ESP. The system filesystem is fixed to ext4. Installer intent uses
the verified managed RAM root and its graphical entry;
validation/bootstrap failures stop without another system root
or shell. The provider build has no product graphical entry and keeps native
admission closed. Start/StartEx/Options, the old
worker and rescue REBUILD_BOOT share a write policy that refuses the legacy
writers even for a Ready native session. Administrative permission alone and
legacy disk slot numbers never authorize the native factory.

The versioned opt-in boot tail preserves the legacy prefix and 96-byte root
binding. The GUI package and system metadata index use page-exclusive EFI
LoaderData allocations; regular system data stays on the source ISO. Before
reading package bytes, kernel staging checks the complete final EFI map,
page-rounded ownership, disjoint map/loader/kernel/boot storage and actual
supervisor HHDM translations. The walker uses the CPU's physical-address width,
NX state and 1-GiB-page capability. A bounded closure walk checks every present
non-leaf branch, including branches unrelated to the queried addresses; shared
tables at the same level are allowed. More than 8192 distinct table pages,
recursive/differently interpreted table pages or unreadable/unsupported table
storage are refused. Boot config, final map and the current four-level paging
tree remain trusted architecture bootstrap storage; this is not validation of
arbitrary hostile page-table pointer graphs. Frame allocation honors firmware
descriptor stride and excludes complete loader and bitmap storage.

When CPUID omits the address-width leaf, actual translations retain a conservative
36-bit limit. Only the ownership closure may tolerate otherwise valid leaf targets
beyond that fallback limit: it never reads those target addresses, and every
non-leaf table pointer still needs strict width, EFI ownership and package-overlap
checks. An explicitly reported CPU width remains strict for leaves as well. NX,
page-size capabilities and reserved-bit checks are unchanged. Serial diagnostics
identify `width_source=cpuid/fallback` and summarize tolerated high leaves with
the first table, level, index and entry; this report does not authorize mappings.

The managed installer RAM root has a 256-MiB admission ceiling: file data plus
1024 bytes per inode (including the implicit PAK1 root) and 4096 base bytes.
The session additionally bounds this by one eighth of allocatable boot RAM.
Retained package record arenas keep their separate 64-MiB ceiling.
These are conservative admission estimates, not allocator reservations. The old
64-MiB ceiling could not admit the 71-MiB GUI root; all existing resources remain
included. ISO preflight reads the shared `installer/root_limits.h` profile and
counts PAK1 records/payload lengths before replacing staging or an existing ISO.
Oversized roots fail with their data, inode estimate and limit; runtime rejection
also logs the actual RAM-derived budget before copying or publishing any root.

Complete PAK1/PAK2 validation uses bounded, retained kernel arenas. The actual
LoadedImage DeviceHandle supplies a bounded device path and real PCI locations;
built-in AHCI/NVMe/IDE/xHCI-BOT registration supplies independent controller and
device generations. A unique matching source registration is required, including its
geometry and bridge path. Read-only 2048-byte source media are distinct from
the factory's 512-byte target profile. Unknown transports, ambiguous/stale
identities and unsupported paths fail closed. Source staging now retains the
real devfs node by registration generation and a whole-source exclusion lease.
Streaming uses authenticated hardware read-only media or a native USB source
guard and bounded kernel worker reads. Older resident media keeps its exclusive
lease owned by the boot TCB. Publication retains this source lifetime to reboot; it does not publish a
Ready installer session or transfer the boot TCB's exclusive I/O authority.

The native target/job service issues monotonic opaque handles bound to
the issuing TCB cookie/TID/PID, with Admin checked before queueing. A temporary
exclusive probe is advisory; the actual factory lease is acquired by its worker
TCB. The service excludes the source physical disk and aliases, stale geometry,
non-512 targets, missing flush support and conflicting leases. Requests support
fresh/full-package installation, with the packaged locale unchanged. Running
progress is published as bounded value snapshots under the job registry lock;
queries never read a concurrently modified factory output. Final status is
published separately after the complete factory returns. Failed ambiguous I/O or quiescent worker death
marks the result unknown and quarantines pins until reboot. The periodic reaper
covers workers in a still-live kernel process as well as before-free hooks.
The version 2 native ABI copies fixed-width requests and replies; it exposes
opaque handles and display metadata, never kernel pointers or raw target IDs.
The UI owns every request on its main TCB. Original-caller Admin admission,
strict effective user-page permissions and same-owner release apply. Worker
arguments remain kernel-owned. Public disk/partition GUIDs and filesystem UUIDs
prefer the shared secure generator, seeded by RDSEED -> virtio-rng -> RDRAND
with bounded retries. If unavailable, the
identifier-only interface diversifies a key from the existing ChaCha20 software
PRNG with wall/HPET time and CPUID-gated TSC. A non-wrapping atomic request
sequence supplies a per-request nonce. These timing inputs are not claimed as cryptographic entropy;
keys, authentication and ASLR require the independent securely seeded generator.
Nonzero/distinct validation and UUID version/variant formatting precede target I/O. The serial
line `installer: identity random source=rdseed`, `virtio-rng`, `rdrand` or `software` records which
path supplied the identifiers without logging their seed material.

A private RAM-tree builder copies the validated PAK1 root, verifies its contents
and required installer executable/config guide, then seals it without mounting
or changing rootdir, /dev or cwd. It admits at most 32768 inodes and 256 MiB of
conservative copied-data/metadata budget, also capped by the session RAM budget;
allocation failures roll back the private tree. Unexpected references fail
closed with retention. It never enters the ordinary tmpfs callbacks. The old
raw-physical prepare API is refused, and launch no longer creates a synthetic
Root user or substitutes a shell. A separate managed RAM backend then publishes
the complete tree atomically, retaining root/cwd and `/dev` identity; boot-owned
handoffs prepare `/pipe`, `/sock` and `/proc`. The verified root package includes
`/system/mod/graphics.sys` in the product. Only its initialized graphics worker
can activate the native session, followed by the standard user-process launcher.
An old standalone FAT32/PAK1 system profile never becomes Ready. Older dual
containers remain readable as ext4 installation sources; they do not enable
FAT32 main-partition installation.

Product and provider installer packaging accept only `INSTALLER_FS=ext4`,
which is also the default. The single system payload keeps links and rich
metadata in PAK2; the separate installer GUI root stays PAK1. `fat32` and `both`
are rejected before staging. Ordinary `vdisk` filesystem options are unchanged.
Build the supported ext4 installer ISO with:

```sh
python3 tools/gen_ninja.py --out build.ninja
./ninja -f build.ninja installer.iso
```

The product ISO exposes `/EFI/BOOT/BOOTX64.EFI` and the complete `/installer`
tree directly in its ISO9660 root. UEFI optical boot (including a VMware virtual
CD/DVD) and raw/DD USB boot share the FAT32 bootstrap `boot/efiboot.img`, exposed
by El Torito and GPT. That bootstrap contains the loader, installer kernel,
installer-root package and index; the large system PAK and independent target
boot files are stored only in the outer ISO tree.

The version 3 index addresses named files rather than build-time FAT sectors.
ISO-mode extraction to a FAT32 USB volume is supported by the native carrier
reader, including noncontiguous and physically backward file cluster chains.
Raw/DD USB and optical sources use the ISO9660 tree on the same authenticated
physical device. The loader needs only firmware FAT support: it reads the
bootstrap before ExitBootServices; native drivers then read the system files.
All FAT32 carrier files and the combined logical stream must fit the existing
4-GiB-minus-one source budget. UEFI is required; this does not add BIOS boot.

The native adapter first retains the actual source disk and USB read-only guard,
then privately resolves the fixed payload/boot paths and compares the source
index byte-for-byte with the firmware-loaded copy before publication. It does
not mount a second filesystem, scan other disks, or weaken source/target leases.
ISO9660 installation data stays read-only; RAM runtime files and the target disk
remain writable. The five-argument compatibility builder still emits resident
or version 1 physical-extent fixtures; it is not the product ISO-mode format.

Native USB sources use the built-in xHCI/BOT driver and logical unit zero.
The loader captures the complete PCI/USB/Hub/interface path and the actual
physical disk and EFI-volume geometry. The native driver independently matches
the controller and bounded port/interface chain before exposing partition nodes.
UEFI zero-based port numbers are normalized from native one-based hub ports;
temporary USB addresses and xHCI slot IDs do not identify the source.

The initial bounded hub scan visits newly registered child hubs before returning,
so installer bootstrap does not depend on workers before the scheduler opens.
Later discovery can wait up to 15 seconds in a sleepable context. Installer-mode
scans retain whole-disk registrations without writable partition aliases, so
GPT edits cannot leave stale partition aliases. Source
provenance is published only after the discovery lease has been released.

If a boot USB source is still absent, native bootstrap performs synchronous hub
rescans before the scheduler opens. It checks identity after each scan, with
100-ms spacing, a 1.5-second elapsed-time guard between probes and at most 15
rescans if the clock is broken. Individual USB operations retain their own
timeouts. Stale or ambiguous identities stop retries immediately; optical boot
does not enter this path.

The boot probe trace records the expected native port's PORTSC, connected-port
mask, first failed phase, completion/timeout information, VID/PID, interface
protocol and latest SCSI status. The installer fatal screen shows these details
without a serial cable. Probe capture stops before workers run; it neither
changes device matching nor grants I/O access. Native probe port numbers are
one-based, while the separately displayed UEFI path remains zero-based.

The selected USB source is software-protected before devfs/partition publication:
its whole-device node has no write callback, and the BOT command path rejects
writes independently, including writes through partition aliases. Hardware
write-protection comes from SCSI MODE SENSE and remains separate from this source
guard. Every retained source read reauthenticates the registered generation and
guard, including after I/O. Disconnect invalidates that identity and quarantines
source/DMA storage until reboot; reinserting a device at the same port cannot
replace the old session. Unknown paths and ambiguous identities remain refused.

For physical-machine failures, a full boot-color cell means that a call returned;
it does not establish that a USB controller or storage device initialized. The
fatal screen now identifies package validation, installation source, private-root
creation and publication separately. USB failures also show the xHCI setup
result, registered storage sources, controller/path match counts and the expected
and first observed port/interface/depth. These are diagnostic snapshots and never
authorize source access or relax identity matching.

The xHCI firmware handoff uses USBLEGSUP at the legacy capability's own address
and USBLEGCTLSTS at offset +4, as specified in xHCI section 7.1. The OS-owned
semaphore is set with a byte access independently of the BIOS-owned semaphore.
An MMIO regression covers absent/chained capabilities, already released firmware,
delayed release, bounded timeout and adjacent-register preservation.

The follow-up `XJ380-installer-usb-fix19.iso` (1,469,569,024 bytes, SHA-256
`e35e5418dab8814876b1a158be91385be727a397ece42010f2db3a4836482949`)
passed the same four QEMU source transports and VMware UEFI optical startup.
Removing the USB source after EFI completed its reads reproduced `-19` with the
real kernel and verified the new on-screen `installation source` stage and zero
registration/match counts. Memory-limit headers were preserved byte for byte.
The physical ThinkPad report established the failed source stage; whether the
firmware-handoff correction resolves that particular machine still needs retesting.

The 2026-11-16 production ISO (1,469,569,024 bytes, SHA-256
`67063449c71ee3ee0056a46a35786b5f9fba675c4108c4ff3eee9dc9614661c3`)
entered the real installer GUI through writable USB 2.0, USB 3.0 and USB-hub
clones, plus optical media, in QEMU/OVMF with 2 GiB RAM. All three USB cases
also passed source-disconnect invalidation. The same exact ISO passed native
installer root publication and user/graphics launch in VMware UEFI with a SATA
CD/DVD and HPET enabled. VMware's screenshot API required guest login, so its
evidence is serial output; QEMU GUI screenshots were inspected. Primary/backup
GPT CRCs, the shared optical/USB FAT volume and all payload stream extents were
independently checked. A separate smaller valid system package completed an
actual USB streaming installation, guest-requested shutdown and independent
Linux readback. Physical ThinkPad boot remains a hardware acceptance check.

Component selection must close over directory ancestors and hardlink
dependencies and revalidate the selected plan; skipping validated records is
unsupported. Existing-target repair, keep-users and in-place updates need their
own conflict/recovery policies; their legacy write paths remain refused.

The older resident UEFI handoff permits **4 GiB per package, 8 GiB combined**. The
product and OpenXJ380 headers share these limits without changing the handoff
ABI. These are admission ceilings, not a promise that every VM has enough RAM.
Packages use page-exclusive contiguous firmware allocations, including RAM
above 4 GiB, within the bootloader's first-512-GiB direct-map alias. Firmware
reads are bounded to 1 MiB per request. A VM using this compatibility path must
have enough contiguous RAM for the packages, firmware, kernel and installer.
Current product media uses the bounded streaming index described below, so the
multi-gigabyte system data file needs no corresponding RAM allocation.

The ISO embeds its packages in a FAT32 EFI image even when the installation
target is ext4. FAT32 limits each file to **4 GiB minus one byte**; a package
of exactly 4 GiB is therefore refused at ISO preflight. The preflight reads
the actual byte ceilings from `include/efi/installer_handoff.h`, checks file
sizes, their combined budget and PAK headers before modifying staging, and
reports the path, actual size and applicable limit. Image sizing uses logical
file lengths with space for growing FAT tables. A failed build preserves the
previous ISO; a completed temporary ISO replaces it atomically.

Boot errors identify the failing package and distinguish missing/open, size,
budget, allocation, truncated read, I/O and close failures. The screen includes
byte counts, limits and the original EFI status. The serial log also records
the successfully loaded byte count and physical address, preserving all 64
bits. Unsupported package formats and invalid handoffs have visible errors.

The initial 2026-11-12 loader regression rebuilt `installer.iso` with a 2,385,364,019-byte
system package. VMware with 4 GiB (IDE CD-ROM) and 8 GiB (SATA CD-ROM) read
every byte and passed both kernel memory ownership proof and complete package
validation. A 2 GiB VMware fixture correctly reported
`PACKAGE_MEMORY_UNAVAILABLE` with EFI status `0x8000000000000009`, rather than
a missing file. The same read-only ISO booted in QEMU/OVMF with 8 GiB, loading
the system package at physical `0x1F1D23000`, retaining the source, publishing
the private root and launching the real installer (PID 1). Host tests covered
64 loader cases, 452 boot-proof checks, six ISO admission tests, six build
profile tests and two session runtime tests.

That initial regression verified package loading and installer entry, not disk
installation. It exposed two later VMware failures: the old IDE driver skipped
ATAPI optical media (`-ENODEV`), and the SATA device-path parser refused the
firmware's direct-port multiplier value zero (`-EOPNOTSUPP`). Those fixtures had
no target disk and were stopped by the host. The old default PAK1 system profile
also displayed the unsupported-ext4 message.

The IDE driver now identifies read-only ATAPI CD/DVD media, obtains real block
geometry with READ CAPACITY, and performs bounded PIO packet reads. It registers
the optical device with its PCI/channel/drive provenance and no write callback.
SATA source parsing preserves both direct-port encodings (zero and `0xffff`).
The zero form matches the AHCI direct-port identity only when the trusted
controller capability says port multipliers are unsupported. Actual multipliers
remain refused; PCI path, geometry, generation and unique source lifetime checks
still apply.

These source errors previously reached the installer's protective `cli; hlt`,
which VMware reports as a guest-disabled CPU after the splash. The fatal gate
now also displays its stage and errno on the framebuffer before halting; the
serial log retains the detailed source-capture stage, EFI status and device path.
The gate is retained for unsafe initialization failures.

The native GUI supports keyboard operation: arrows select targets, Enter opens
review, Escape returns, and F8 submits only from the review page. Enter alone
cannot erase a disk. F2 changes only the displayed locale; F5 refreshes targets.
After verified durable success, F12 requests normal shutdown. Callback events
remain queued for the owning main TCB, and input from an older displayed frame
cannot submit a new confirmation page.

AHCI completion also waits for the final task-file BSY/DRQ bits to clear after
PxCI clears, under the original command deadline. This retains DMA ownership
while a virtual controller finishes publishing status. Device/transport errors,
unexpected active tags, short transfers and expired deadlines still fail closed;
the driver never replays an ambiguous write. Diagnostics record the sampled
pending task-file value, with bounded serial output during large imports.

The completed 2026-11-12 VMware regression rebuilt the default ISO with a
1,280,698,845-byte PAK2 system package (17,226 records, 1,278,879,402 data bytes).
VMware Workstation 17.6.4, UEFI, 4 GiB RAM and four vCPUs completed both IDE
ATAPI and SATA optical-source installations onto separate new 16 GiB SATA disks.
Both real GUI jobs reached durable activation and displayed success; F12 then
powered off the guests normally. No guest-disabled-CPU event occurred.

Independent readback after that shutdown passed GPT/binding checks, `e2fsck -fn`
and `fsck.fat -n`. Linux `debugfs` verified every record's namespace, file hash,
mode, owner, mtime, link identity/target and xattrs against the frozen package;
the installed EFI and kernel matched their build inputs byte for byte. Neither
root needed journal recovery. After removing the ISO, both installed disks
cold-booted through bound-root verification and mounted ext4 successfully.
First-admin creation, desktop/dock login and a normal calculator launch through
Win+R also passed. These final desktop smoke guests were stopped by the host
after evidence capture; the earlier installation shutdowns were guest initiated.
The same final ISO reached the native installer in QEMU/OVMF with 4 GiB/four
vCPUs, and a separate ordinary FAT32 `vdisk` build passed.

The tested ISO was 1,362,573,312 bytes, SHA-256
`bdc2b8d73ac684ae040a90a1c42656840b1cfdc30d5abc02dc05c898df569302`.
All destructive tests used newly created disposable disks; no existing user VM
disk was used. These are bounded controller/configuration tests, not a claim of
support for arbitrary VMware controllers, port multipliers or all hardware.

## Verification

From the parent project, the whole-disk additions have these focused commands:

```sh
python3 -m unittest discover -s tests -p test_installer_disk_layout.py -v
python3 -m unittest discover -s tests -p test_installer_esp.py -v
python3 -m unittest discover -s tests -p test_installer_esp_verify.py -v
python3 -m unittest discover -s tests -p test_installer_ext4_verify.py -v
python3 -m unittest discover -s tests -p test_installer_factory.py -v
python3 tests/test_ide_atapi.py
python3 tests/test_ahci_flush.py
python3 tests/test_block_source.py
python3 tests/test_installer_loader.py
python3 tests/test_installer_iso_preflight.py
python3 tests/test_installer_iso_hybrid.py
python3 tests/test_usb_installer_guard.py
python3 tests/test_xhci_legacy_handoff.py
python3 tests/test_usb_boot_probe.py
# Boot one exact product ISO through writable USB clones and optical media:
python3 tests/run_installer_usb_qemu.py --source-iso out/XJ380-installer-usb.iso --disconnect
# Delay native USB arrival until the bootstrap rescan, then verify GUI entry:
python3 tests/run_installer_usb_qemu.py --source-iso out/XJ380-installer-usb.iso --late-source --disconnect
# Check the same ISO in a new VMware UEFI CD/DVD VM (Windows/WSL with vmrun):
python3 tests/run_installer_vmware_boot.py --iso out/XJ380-installer-usb.iso
python3 -m unittest discover -s tests -p test_installer_build_profile.py -v
python3 tests/test_installer_native_ui.py
# After a completed product build, on disposable targets:
python3 tests/run_native_installer_window_qemu.py --boot-image /tmp/production-fixture/fat32.img --profile smoke
python3 tests/run_native_installer_window_qemu.py --boot-image /tmp/production-fixture/fat32.img --profile full --boot-smoke
```

The USB runner freezes the ISO and extracts that image's own EFI, kernel and
packages. It uses ordinary writable USB 2.0, USB 3.0 and hub-connected clones,
checks native GUI entry, and can check disconnect invalidation. Its default
scope ends at the GUI; `--install` additionally uses the real installation
worker, guest-requested shutdown and independent Linux target readback. The
VMware runner creates its own disposable VM/target, checks native GUI entry,
attempts a screenshot and stops only that VM. Neither runner writes a physical
USB drive or an existing VM disk. Emulator results do not establish physical
ThinkPad compatibility.

`python3 tests/test_installer_package.py -v` compiles the actual production
validator with ASan/UBSan. It checks malformed/truncated fields, repaired-CRC
mutations, paths and explicit directory parents, hardlink identities, opaque
symlink cycles, non-UTF8 bytes, timestamp boundaries, xattrs, budgets and caller
arena boundaries. A real host tree is encoded and decoded with exact metadata,
data CRC, raw names and link identity comparisons. Default PAK1 and explicit
PAK2 CLI behavior, skipped-link diagnostics, invalid late inputs, staged output
refusal, and injected host fsync/replace failures are tested. The isolated
freestanding object has no undefined symbols in the tested x86_64 build.

LeakSanitizer is disabled because of the sandbox tracing limitation; no leak
sanitizer claim is made. This is bounded structural testing, not exhaustive
fuzzing or an independent security/license audit. The native importer tests use
actual formatter/application bodies, Linux readback/fsck and selected failed-I/O
persistence states with independent native/Linux recovery. Adapter tests execute
the actual guard, device-identity and block-lease code, including allocation
failure, alias conflicts, first-error quarantine and quiescent owner death.
The actual-module factory host harness passed four methods/21 scenarios with
thirteen coordinator allocation failures, ten selected backend cuts and five
activation persistence models, including reused-GUID stale-binding refusal.
The full ext4 verifier passed eight methods/41 command checks, including
independent Linux mutations. These bounded models are separate from normal
completion tests and do not cover arbitrary torn sectors or all hardware caches.

The earlier trusted whole-disk QEMU runs used product kernel SHA-256
`268e8f8f6903c2244deb11502954dff3e23de7b0f131727962548f0fa281c913`.
Smoke and full profiles each passed 20 kernel checks per AHCI/NVMe controller,
then shut the guest down normally. The full profile has 277 records and
123,951,877 regular-file bytes, containing the system stage without Linux
compatibility packages plus metadata/link diagnostics. Independent Linux checks
matched the complete manifest, GPT, root/ESP filesystems, kernel/EFI payloads
and binding. Full runs took approximately 185.300/183.937 seconds. Separate
unmodified production cold boots reached login process creation on both targets;
those short checks stop by host quit and claim no clean shutdown or completed
GUI login. See `/tmp/xj380-native-installer-window-qemu-validation.json` and
[the ext4 status](EXT4.md) for exact scope. That evidence excluded public user-facing installer acceptance; resource
reservation and protected-xattr enforcement have their own tests. Shared
file mappings remain unsupported.


Earlier boot-staging checks used product kernel `eb003e84beb88a3d...` and
production EFI, with no installer test hook: AHCI PAK1, NVMe PAK2, read-only
2048-byte ATAPI PAK2, and damaged-PAK2 refusal all passed. At that revision,
installer intent stopped before root mounting and native user entry was unavailable. See
[the current regression record](EXT4.md#current-boot-staging-and-vfs-recovery-regression)
for exact hashes, writable-source changes, recovery tests and scope.

```sh
python3 -m unittest discover -s tests -p test_boot_memory.py -v
python3 -m unittest discover -s tests -p test_installer_boot_proof.py -v
python3 -m unittest discover -s tests -p test_block_source.py -v
python3 -m unittest discover -s tests -p test_installer_loader.py -v
python3 -m unittest discover -s tests -p test_installer_build_profile.py -v
python3 -m unittest discover -s tests -p test_installer_iso_preflight.py -v
python3 -m unittest discover -s tests -p test_installer_entry_gates.py -v
python3 -m unittest discover -s tests -p test_installer_session_runtime.py -v
python3 -m unittest discover -s tests -p test_ahci_capacity.py -v
# Consumes completed builds and an unbound FAT32 boot fixture; only clones are modified.
python3 tests/run_installer_entry_qemu.py --boot-image /tmp/production-fixture/fat32.img --root-pak /tmp/owned-stage/installer-root.pak
```


For the later retained-source/private-root phase, see
[the current source and handle record](EXT4.md#retained-sources-kernel-handles-and-private-root).
That historical record used kernel prefix `d85330ad`, with the root sealed
but unpublished. The later native ABI/managed-root phase is documented in
[the current integration record](EXT4.md#managed-installer-root-and-native-user-requests). To exercise the complete GUI-root
PAK1 fixture after both full builds, pass it explicitly:

```sh
python3 tests/run_installer_entry_qemu.py --boot-image /tmp/production-fixture/fat32.img --root-pak /tmp/owned-stage/installer-root.pak
```


Production GUI/native factory acceptance uses separately prepared current inputs:

```sh
python3 tests/run_installer_native_qemu.py all --work /tmp/owned-native-acceptance --root-pak /tmp/owned-stage/installer-root.pak --system-pak /tmp/owned-stage/system-payload.pak
```

It builds nothing and uses no probe kernel or Ready override. Read-only optical
source media and initially zero 768 MiB target disks stay under `/tmp`; each
AHCI/NVMe case must observe native durable GUI success, actual guest shutdown,
then independent Linux GPT/ext4/FAT/binding and complete manifest readback.
A failed case is preserved and cannot automatically restart. `interact` steps
allow screenshots and individual controls; a host stop never counts as a clean
guest shutdown or successful installation.


Earlier production user-facing acceptance passed on AHCI and NVMe, each with a
complete 268-record / 125,144,042-data-byte native payload, real kernel worker,
durable GUI success, actual guest shutdown and Linux manifest/GPT/ext4/FAT/binding
readback. Both use the final kernel prefix `6f832c33`; source ISOs remain unchanged.
See [the integration evidence](EXT4.md#managed-installer-root-and-native-user-requests)
for fixture scope, timings, source provenance and unsupported modes.

The installed output also passed real first-account creation on AHCI followed by
password login on NVMe using the same unchanged-artifact clone, 15 file-manager
launches, Control Center and two UI guest shutdowns. Linux checking after normal
use returned zero with clean journal/orphan state and persistent account/profile
files. This is fresh installation and normal persistence evidence, separate
from arbitrary interrupted-upgrade or installation power-cut recovery.


## File-addressed installation media (stream-index version 3)

The 128-byte header and `XJSTRM01` magic are retained. Version 3 uses the final
24-byte table as `{logical offset, fixed file ID, real file length}`. ID zero is
`/installer/system-payload.pak`; IDs one and two are the independent
`/installer/boot-files/EFI/XJ380/BOOTX64.EFI` and `kernel.krl`. There is exactly
one entry without an independent boot catalog, or exactly three with one.
Ranges cover the logical source without padding or gaps. Metadata still stores
file-relative offsets, lengths and CRC32Cs, and source data is checked before
any destructive target request. The existing handoff profile and fixed ABI
prefix remain unchanged.

After source retention, a bounded private FAT32 reader locates files inside the
actual loaded EFI volume. If its system PAK is absent, the original allocation-
free ISO9660 core resolves files in the same retained whole device. A present
but invalid FAT payload does not fall back to another source. FAT chains must
terminate at exactly the declared size; mirror mismatches, cycles, crosslinked
source files and out-of-volume clusters are refused. At most 65536 FAT extents
are retained, ordered logically rather than by physical address. Carrier
metadata workspace is bounded independently of system-data size. Parsing and
I/O run outside publication locks; workers use the existing single-reader
admission and 64-KiB bounce buffer. The one-time metadata preparation also admits
the retained BSP boot-owner idle TCB, whose identity and TID must match. Its
preparation flag is cleared before publication; application tasks, foreign idle
TCBs and a reused TID cannot acquire that bootstrap exception. There are no
firmware calls after handoff.

`tests/test_installer_source_media.py` links the production FAT/ISO carrier
reader under ASan/UBSan, including a real product hybrid and backward
fragmentation. The USB runner's `usb-iso` mode creates a fresh GPT/FAT32 regular
image and copies the exact ISO's visible files into it; it tests extraction
semantics, not the Rufus application UI. Use `--install --cold-boot` with
`--modes usb usb-iso optical` for full guest installation, independent target
readback and installed-system boot. The source ISO and every test input are
frozen before running; only disposable image clones are changed.

The 2026-11-21 production-image acceptance completed extraction-based FAT32 USB,
raw/DD USB and UEFI optical installation with the same frozen ISO. QEMU used TCG,
2 GiB RAM and a 4 GiB NVMe target. Each case installed all 16900 records, passed
independent GPT/FAT/ext4 checks and full content/metadata/link/xattr readback,
then booted its target clone through the registered vendor entry with the
installer detached and the generic fallback loader renamed. Rufus UI and
physical USB/DVD hardware were not exercised by this virtual-media acceptance.

## Single ext4 payload and bounded source reads

The product ISO contains one ext4 PAK2 in `out/system-payload.pak` and a small
`system-index.pak`. The index replaces regular-file data with source offsets,
lengths and CRC32Cs while retaining namespace, inode identity, hardlinks,
symlink text, mode, owner, timestamps and xattrs. Removing the duplicate FAT32
system payload reduces the ISO without flattening links or losing metadata.
The target has an ext4 system root first and a separate 64 MiB FAT32 ESP last.

The loader reads only the index and the small GUI package. Handoff profile 4
retains the existing 880-byte BOOT_CONFIG layout. The index is bounded to 64 MiB;
the legacy physical index's 128-byte header contains magic `XJSTRM01`, u32 version 1, u32 header size
128, then u64 index bytes, on-media data-file bytes, extent count, ext4 metadata
offset/bytes, legacy FAT32 metadata offset/bytes and extent-table offset. The
legacy metadata length is zero on current media. Bytes 80..119 are zero; body
CRC32C is at 120 and the first 124 header bytes' CRC32C is at 124. The bounded
24-byte `{logical offset, EFI-volume byte offset, length}` table exhausts the
source file with increasing, nonoverlapping physical extents inside the
loaded EFI volume. Optional FAT32 metadata in older indexes remains parseable
but is not admitted or published as an installation capability.

Each regular file uses a 24-byte `{u64 source offset, u64 real length,
u32 CRC32C, u32 zero}` descriptor. The original PAK2 namespace and metadata
validator checks the ext4 plan. File and total-data budgets apply after
expanding the descriptors. No regular-file data is read during plan admission.
Older resident dual containers remain accepted only as ext4 sources. Their
ext4 subspan shares the proven parent allocation and lifetime; the kernel
allocates no second FAT32 record arena.

The source keeps a whole-device exclusion lease and a real devfs reference.
Streaming requires authenticated hardware read-only media or the built-in USB
source guard, together with an unwritable node. A nonexclusive exclusion lease
permits trusted kernel workers' reads
while blocking overlapping filesystem leases, deletion and re-registration.
A bounded reader uses a 64 KiB aligned bounce buffer for 512/2048-byte devices;
consumers use at most 128 KiB per chunk. No firmware handles survive
ExitBootServices. Memory use does not scale with the on-media system data.

Before the first target request, the factory checks all source data CRCs.
Copying and independent readback check file CRCs again and compare complete
target contents. Source failures stop callbacks and retain quarantine/lifetime
rules. Only the small loader/kernel payload for the ESP is retained. The
acceptance configuration is 2 GiB, with separate 3 GiB installed-boot coverage.
The native UI offers no filesystem selector; INFO advertises only ext4, and
SUBMIT/job admission reject FAT32 before destructive target I/O. Existing FAT
filesystem drivers, legacy disk compatibility and the FAT32 ESP are retained.

## Native installation progress

Native ABI version 2 extends QUERY with a stage, completed/total work units and
an overall fraction in thousandths. `installer/progress.h` defines the shared
kernel callback and phase weights. Formatting counts completed metadata work;
copying counts accepted/committed data; directory, link and metadata phases
count scanned/completed manifest work; verification counts real readback work.
Long operations report from the worker's private state. The callback only
publishes a small value snapshot, with no allocation, VFS call, rendering or
wait under the registry spin lock. UI callbacks still enqueue input, and the
main UI thread owns every request/handle.

The GUI polls approximately every 500 ms and displays a bar, stage and completed
work. Weighted percentages describe phase work, not elapsed-time estimates.
Progress is monotonic per job and remains below 100% until terminal success
and durable activation. A query failure retains the last valid snapshot and
shows the existing unknown-running warning; a failed or quarantined job never
becomes a successful 100% display.

Validation entry points include `test_installer_build_profile.py`,
`test_installer_stream_index.py`, `test_installer_session_runtime.py` and the
native UI/service/job regressions. The stream-index test binds a real single
ext4 PAK2 into a FAT source image, then checks native indexed reads and repaired
CRC/invalid-extent rejection under ASan/UBSan. Historical dual-container and
private FAT factory tests remain compatibility checks.

`tests/run_installer_native_qemu.py` accepts `--source-iso`, `--filesystem ext4`,
`--target-mib`, `--memory-mib` (default 2048), and frozen `--kernel`/`--efi` inputs.
Prepare a fresh owned directory and keep the runner in the foreground during
its VM lifetime:

```sh
python3 tests/run_installer_native_qemu.py prepare --work /tmp/owned-ext4-validation \
  --root-pak out/installer-root.pak --system-pak out/system-payload.pak \
  --source-iso XJ380-installer.iso --target-mib 8192 --buses ahci --keyboard
python3 tests/run_installer_native_qemu.py run --work /tmp/owned-ext4-validation --bus ahci
python3 tests/run_installed_gui_qemu.py --install-work /tmp/owned-ext4-validation \
  --work /tmp/owned-ext4-cold-boot
```

The offline verifier compares the complete namespace, contents and supported
metadata and runs Linux filesystem checks after guest shutdown. The cold-boot
runner attaches no ISO and checks first-user creation/login and normal app
launching. Its final host QMP quit is recorded explicitly, not as guest
shutdown. Credentials stay in private disposable files outside exported
reports. Use `--memory-mib 3072` for separate 3 GiB boot coverage.

## 2026-11-12 ext4-only ISO before Launcher repair

The 2026-11-12 ext4-only ISO is 1,366,771,712 bytes (1.273 GiB), compared with
3,802,613,760 bytes for the preceding dual image: a 64.1% reduction. SHA-256:
`617bda44b8af417d19cfaba928432c2f2cd89eaddd0626e9e91317d33dceb641`.
Its only system data file is a 1,280,898,532-byte PAK2. The 2,188,371-byte
stream index has one source extent and zero FAT32 system metadata bytes.
Independent extraction checks the ISO's exact EFI, kernel, GUI and system
package hashes against frozen build inputs. The default workspace
`XJ380-installer.iso` matches this frozen ISO; it was published after the
previous running VM released its optical-image handle.

The complete `./ninja all` build passed and left the frozen kernel/EFI/GUI
inputs unchanged. All 32 selected hosted tests passed, including ext4-only
source mapping with native ASan/UBSan reads and rejection of FAT32 installation
requests. Compatibility coverage includes older dual indexes; current product
packaging accepts only ext4.

The same frozen ISO completed a full native installation in VMware with UEFI,
four vCPUs, 2 GiB RAM and a verified blank 8 GiB SATA target. The real GUI reached
successful durable activation with 17,231 records and 1,279,078,593 data bytes,
then the guest powered off normally. Startup logs show exactly 34,773,277 GUI
package bytes and 2,188,371 index bytes read by firmware; the full system data
file stayed on the medium. Screenshots record actual source checks, file copy,
metadata application, system readback and final 100% success.

Independent Linux checking returned zero for GPT, ext4 and ESP, with no
journal replay or orphan cleanup needed. All 17,231 canonical records matched
namespace, data, mode, owner, timestamps, hardlinks, symlinks and xattrs; kernel,
EFI and boot-binding identities matched exactly. With no ISO attached, the same
installed disk passed 2 GiB first-account creation, desktop, calculator and
file-manager use, then normal guest shutdown. At 3 GiB it passed existing-account
password login, bound ext4 root mount, desktop and the first calculator launch.

The additional 3 GiB Launcher repeat-launch check **did not pass**: a second
calculator process was created without a user thread, and subsequent Launcher
ELF reads returned `-5`. The Launcher handles Enter through both MSG_SPCHAR and
MSG_KEYDOWN callbacks and exits from another thread; duplicate dispatch racing
exit is a suspected cause of abandoned ext4 I/O, not a confirmed disk-content
fault. These Launcher/normal-root paths were not edited for the installer
change. The VM was suspended to preserve the failure, not recorded as normal
shutdown. The older dual-ISO 3 GiB two-calculator evidence used Win+R/elfrun,
followed by host hard-stop, and does not validate this failing Launcher path.
The current ISO's installer/boot results must not be presented as passing that
additional application check. Logs and the suspended fixture are retained in
`installer-ext4-only-validation/vmware-ext4-only-2g` beside the frozen inputs.

## 2026-11-13 Launcher and global shortcut repair

The follow-up fixes two independent runtime paths exposed by repeated application
launches. The Launcher previously activated both the Enter key notification and
its paired text notification. It also performed launch/history I/O in a callback
TCB while main could exit. Callbacks now only publish input to a fixed 32-entry
queue. Main owns query updates, search, launch, history writes, power actions and
window close. Only the key notification activates an entry. Main finishes that
operation before closing its window and returning through normal XAPI exit.
Overflow cancels the transient Launcher rather than activate after lost navigation.

The first follow-up ISO, SHA-256
`1efca68cc50cb98f689e4048453aed0fcf370bbb6ad6d1d57127d4cbefa14fe4`,
passed full native installation and pristine manifest readback, but **failed**
the additional repeat-launch runtime check. Its suspended memory showed intact
ext4 I/O/error latches and inode mappings. A separate diagnostic clone identified
actual VFS `EACCES`, previously discarded by the ELF loader and reported as `EIO`.
Win+Space, Win+R and related actions loaded applications directly in the keyboard
IRQ, inheriting whichever application's task/permission context was interrupted.
They now only enqueue bounded action values under a short IRQ-saving spin lock.
The existing graphics worker pops those actions before executing launch/window
operations, outside the queue lock. Keyboard code belongs to `graphics.sys`, so
validation verifies that installed module as well as the kernel and Launcher.
The ELF loader now preserves the VFS errno and reports an incomplete-read diagnostic.

Neither repair weakens application permissions or the native ext4 abandonment
rules. There is no application single-instance rule or launch-rate limit. The
bounded shortcut queue preserves separate presses until full; overflow never
executes work in an IRQ. PS/2 and USB share this path.

All 47 selected hosted tests passed, including production Rust callback/main-loop
concurrency, paired Enter, failed-launch retry, fresh Launcher instances, overflow,
queue wraparound, the production PS/2/USB shortcut state machine, worker-only
launch/window execution outside locks, and ELF denied/short-read cleanup with
ASan/UBSan. Installer streaming, UI, service, loading, build profile and permission
regressions are included. Final ISO/native acceptance details follow below.

The final rebuilt ISO is 1,366,771,712 bytes (about 1.27 GiB), SHA-256
`101bc4c32851311c8b3eae6c5e376ddf94b674496e66d03d6ea174a11a15484a`.
Its sole system PAK2 is 1,280,907,904 bytes. The resident stream index is
2,188,371 bytes with one on-media extent and zero FAT32 system metadata.
Independent extraction verifies the ISO carrier's EFI/kernel/GUI/system payload
identities and the exact Launcher and graphics module bytes in the system PAK2.
The GUI package is 34,775,553 bytes; the system payload remains on the medium.

Final native acceptance uses a new initially zero 8 GiB SATA disk, VMware UEFI,
four CPUs and 2048 MiB RAM. The production installer reached green 100%/phase12
for all 17,231 records and 1,279,087,965 data bytes, followed by guest F12 S5.
An independent Linux readback verifies exact namespace, content, mode, owner,
mtime, hardlinks, symlinks and xattrs, GPT CRCs/identities, ESP and root binding,
and exact EFI/kernel bytes. Both filesystem checks returned zero, with no
journal recovery requirement or orphan. No host ext4 formatter, Ready override
or probe kernel was used on the acceptance installation.

The same installed disk then passed both ordinary ISO-free boot sessions:

| Guest RAM | Account and desktop | Application checks | Shutdown |
| --- | --- | --- | --- |
| 2 GiB | First administrator creation and desktop | Five simultaneous calculators, two file managers, task manager, Win+R and a subsequent Launcher | Guest menu S5 |
| 3 GiB | Existing-account password login and retained recent items | The same repeated application checks | Guest menu S5 |

Each calculator Enter produced exactly one process and one user thread. Every
listed application had the expected successful-start count and no unexpected
exit; later Launcher loads continued to succeed. After both sessions, another
independent Linux readback passed GPT/root binding, ext4/ESP fsck (both zero),
no recovery/orphans, and exact frozen Launcher, graphics module and kernel
hashes. The original failure fixtures remain preserved separately. Acceptance
reports and screenshots are in `installer-launcher-final-validation` under the
local validation artifact directory; no test VM was shared with the other chat.

## Historical dual-filesystem ISO validation (superseded)

The complete 2026-11-12 product ISO passed both filesystem paths in VMware
Workstation with 2048 MiB RAM, four CPUs, an IDE optical source and separate
initially zero 8 GiB SATA target disks. The exact ISO SHA-256 is
`2584f3b81147f9dcfb704f4b581d541023f3fb30a22324841a2cd532a9254415`.
Both runs used the production EFI/kernel, ordinary filesystem selection and
F8 submission, actual green durable success, guest F12 shutdown, then independent
Linux GPT/filesystem/ESP/binding and complete manifest readback.

| Root filesystem | Complete manifest records | 2 GiB disk-only first boot | 3 GiB disk-only reboot |
| --- | ---: | --- | --- |
| FAT32 | 22,879 including logical root | Account creation, desktop, calculator and file manager | Existing-account password login and desktop |
| ext4 | 17,231 | Account creation, desktop, calculator and file manager | Existing-account password login, desktop and concurrent calculators |

Both installed disks booted with the ISO removed. FAT32 also completed normal
guest shutdown after both desktop sessions; the ext4 desktop checks ended with
an explicitly recorded host stop. All filesystem checks returned zero. These
are fresh installation and ordinary GUI/persistence checks on the stated virtual
hardware; they do not establish arbitrary interrupted-upgrade recovery.

The loader read only the 34,778,533-byte GUI package and 5,164,967-byte index;
the 3,666,484,176-byte dual system data file stayed on the ISO. FAT32 installed
and read back 2,384,225,201 regular-file bytes on the 2 GiB guest. The IDE reader
uses bounded bulk PIO, including odd-phase and multi-packet checks, and the
private FAT root cache has separate writer/verifier lifetimes. Earlier loading,
interrupted-host and pre-cache diagnostics are not final acceptance results.

The FAT32 Linux export oracle explicitly selects `LC_ALL=C.UTF-8` for mtools;
the generic command oracle's C locale otherwise substitutes underscores for
valid Unicode long filenames. The Unicode export regression checks actual FAT
names and contents. The complete final disk was rechecked against the unchanged
original manifest after this host-tool correction, without repairing the disk.
