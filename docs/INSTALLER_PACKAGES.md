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
trusted `installer_build_ext4_disk` kernel factory already consumes PAK2 through
the fresh-volume importer described below; the installer boot route remains
disabled. Metadata represented in a package is distinct from metadata actually
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

The arena is caller-owned, non-overlapping storage for 96-byte Records on the
x86_64 host tested here; it has no kernel allocation dependency. A 262144-record
arena requires approximately 24 MiB. All pointers are borrowed. The caller must
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

The product native user installer supports a fresh full-package ext4 root
with a FAT32 ESP. Installer intent uses the verified managed RAM root and its
graphical entry; validation/bootstrap failures stop without another system root
or shell. The provider build has no product graphical entry and keeps native
admission closed. Start/StartEx/Options, the old
worker and rescue REBUILD_BOOT share a write policy that refuses the legacy
writers even for a Ready native session. Administrative permission alone and
legacy disk slot numbers never authorize the native factory.

The versioned opt-in boot tail preserves the legacy prefix and 96-byte root
binding. Both packages use page-exclusive EFI LoaderData allocations. Before
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

Complete PAK1/PAK2 validation uses bounded, retained kernel arenas. The actual
LoadedImage DeviceHandle supplies a bounded device path and real PCI locations;
built-in AHCI/NVMe/IDE registration supplies independent controller and device
generations. A unique matching source registration is required, including its
geometry and bridge path. Read-only 2048-byte source media are distinct from
the factory's 512-byte target profile. Unknown transports, ambiguous/stale
identities and unsupported paths fail closed. Source staging now retains the
real devfs node by registration generation and an exclusive whole-source lease
owned by the boot TCB. Publication of this source lifetime retains it to reboot;
it does not publish a Ready installer session. Cross-TCB checks may validate
the source lease but cannot use or transfer its I/O authority.

The native target/job service issues monotonic opaque handles bound to
the issuing TCB cookie/TID/PID, with Admin checked before queueing. A temporary
exclusive probe is advisory; the actual factory lease is acquired by its worker
TCB. The service excludes the source physical disk and aliases, stale geometry,
non-512 targets, missing flush support and conflicting leases. Requests support
only fresh/full-package ext4, with the packaged locale unchanged. Running status
does not read a concurrently modified factory result; final snapshots are
published under a short lock. Failed ambiguous I/O or quiescent worker death
marks the result unknown and quarantines pins until reboot. The periodic reaper
covers workers in a still-live kernel process as well as before-free hooks.
The version 1 native ABI copies fixed-width requests and replies; it exposes
opaque handles and display metadata, never kernel pointers or raw target IDs.
The UI owns every request on its main TCB. Original-caller Admin admission,
strict effective user-page permissions and same-owner release apply. Worker
arguments remain kernel-owned. Checked hardware-only entropy supplies new
identities; entropy failure issues no target I/O.

A private RAM-tree builder copies the validated PAK1 root, verifies its contents
and required installer executable/config guide, then seals it without mounting
or changing rootdir, /dev or cwd. It admits at most 32768 inodes and 64 MiB of
conservative copied-data/metadata budget, also capped by the session RAM budget;
allocation failures roll back the private tree. Unexpected references fail
closed with retention. It never enters the ordinary tmpfs callbacks. The old
raw-physical prepare API is refused, and launch no longer creates a synthetic
Root user or substitutes a shell. A separate managed RAM backend then publishes
the complete tree atomically, retaining root/cwd and `/dev` identity; boot-owned
handoffs prepare `/pipe`, `/sock` and `/proc`. The verified root package includes
`/system/mod/graphics.sys` in the product. Only its initialized graphics worker
can activate the native session, followed by the standard user-process launcher.
The unsupported FAT32/PAK1 system profile never becomes Ready.

Installer packaging now accepts `INSTALLER_FS=fat32|ext4`, default `fat32`.
The default system payload remains FAT32-compatible PAK1; explicit ext4 keeps
links and rich metadata in PAK2. The installer GUI root always stays PAK1.
For example, use the same environment when generating and running Ninja:

```sh
INSTALLER_FS=ext4 python3 tools/gen_ninja.py --out build.ninja
INSTALLER_FS=ext4 ninja -f build.ninja installer.iso
```

This creates an installer package profile, not an enabled user installation
service. Component selection must close over directory ancestors and hardlink
dependencies and revalidate the selected plan; skipping validated records is
unsupported. Existing-target repair, keep-users and in-place updates need their
own conflict/recovery policies; their legacy write paths remain refused.

## Verification

From the parent project, the whole-disk additions have these focused commands:

```sh
python3 -m unittest discover -s tests -p test_installer_disk_layout.py -v
python3 -m unittest discover -s tests -p test_installer_esp.py -v
python3 -m unittest discover -s tests -p test_installer_esp_verify.py -v
python3 -m unittest discover -s tests -p test_installer_ext4_verify.py -v
python3 -m unittest discover -s tests -p test_installer_factory.py -v
# After a completed product build, on disposable targets:
python3 tests/run_native_installer_window_qemu.py --boot-image /tmp/production-fixture/fat32.img --profile smoke
python3 tests/run_native_installer_window_qemu.py --boot-image /tmp/production-fixture/fat32.img --profile full --boot-smoke
```

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


Current production user-facing acceptance passed on AHCI and NVMe, each with a
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
