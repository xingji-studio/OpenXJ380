# ext4 dependency license audit and implementation decision

Review date: 2026-10-30. Scope: the replacement ext4 kernel implementation in
OpenXJ380 and its integration into the closed-source XJ380 parent.

## Decision

Use independently authored native C++ code in OpenXJ380 under Apache-2.0.
**No external ext4 implementation is approved for import or linkage by this
review.** The reviewed candidates below remain unapproved; a package license
field or a repository badge is not a completed per-file/provenance audit.

This decision preserves the project's licensing boundary by introducing no new
third-party filesystem implementation. Existing project dependency exceptions
remain separate. It does not certify the provenance of future code: each change
still requires review of its actual origin and content. An Apache SPDX header
cannot make copied or translated copyleft implementation code permissive.

The implementation must use on-disk format facts and independently generated
fixtures. Do not copy, translate, or adapt Linux/lwext4 implementation code into
the replacement. Do not describe this review as a formal clean-room process or
as a whole-project copyright clearance.

## What was actually reviewed

Public upstream file views and license declarations were inspected. Complete
repository archives, immutable revisions, complete source-file inventories, and
resolved transitive dependency closures were **not** obtained in this audit
environment. The URLs below refer to moving upstream branches; they are evidence
for these limited observations, not approved source pins. No fixed revision or
source hash is asserted. No candidate source was imported into either repository.

| Candidate | Confirmed source-level observations | Outstanding review / engineering limits | Decision |
| --- | --- | --- | --- |
| ext4-view-rs | The inspected `Cargo.toml` has an Apache-2.0/MIT file header and a workspace license expression `MIT OR Apache-2.0`; its library dependencies list `bitflags` and `crc`. | All implementation file headers, dependency source licenses, and provenance still require a pinned review. Upstream explicitly excludes write support; it cannot supply the requested writable root. | Not imported or approved. |
| ext4plus | Upstream declares Apache-2.0 OR MIT and identifies its origin as a fork of ext4-view-rs. | No complete per-file or transitive license audit was completed. Upstream reports beta write bugs, missing journal writes, and inode-body-only xattr writes. | Not imported or approved. |
| FreeBSD ext2fs | The actual `ext2_extents.c` and `ext2_extattr.c` source headers each declare BSD-2-Clause and retain author/copyright notices. | These two files do not establish the license of the entire subtree, its headers, or FreeBSD kernel dependencies. A full dependency/provenance and feature review is required before any port. | Not imported or approved. |
| antimatter-studios/rust-fs-ext4 | `Cargo.toml` declares MIT, lists runtime dependencies and a sibling path dependency on rust-fs-core, and requires unwind panic profiles for the FFI boundary. | No full per-file, provenance, lockfile, or dependency audit was completed. The current hosted runtime/FFI design is not a drop-in freestanding kernel backend. | Not imported or approved. |

Source references supporting the table:

- [ext4-view Cargo manifest and its own license header](https://github.com/nicholasbishop/ext4-view-rs/blob/main/Cargo.toml).
- [ext4-view design goals and read-only scope](https://github.com/nicholasbishop/ext4-view-rs#design-goals).
- [ext4plus upstream limitations and license statement](https://github.com/arihant2math/ext4plus#limitations).
- [FreeBSD extent source with BSD-2-Clause notice](https://github.com/freebsd/freebsd-src/blob/main/sys/fs/ext2fs/ext2_extents.c).
- [FreeBSD extended-attribute source with BSD-2-Clause notice](https://github.com/freebsd/freebsd-src/blob/main/sys/fs/ext2fs/ext2_extattr.c).
- [rust-fs-ext4 Cargo manifest](https://github.com/antimatter-studios/rust-fs-ext4/blob/main/Cargo.toml).

For rust-fs-ext4 specifically, the inspected manifest lists `crc32c`, `bitflags`,
`unicode-normalization`, `caseless`, and `rust-fs-core` as library dependencies.
The last uses a sibling source path and version 0.4.2, so the package manifest
alone cannot establish the dependency closure. Both release and development
profiles select unwind; the release comment explicitly attributes this to FFI
panic catching. CLI dependencies are feature-gated and must not be confused with
the library dependencies. A `catch_unwind` found in the inspected `src/lib.rs`
is inside a test-only module and is **not** itself evidence of production use.
The manifest's explicit FFI constraint is the relevant evidence here.

## Approved implementation inputs

The approved route has an empty new third-party implementation dependency set.
New first-party files need Apache-2.0 SPDX identifiers and a provenance record
that identifies the public format descriptions used. Existing VFS/block I/O,
allocator, mutex, and ABI support remains part of the existing kernel boundary.
This review does not expand that boundary's license exceptions.

Use the [ext4 on-disk format documentation](https://www.kernel.org/doc/html/latest/filesystems/ext4/)
as a description of storage fields and behavior. Record references to format
facts; do not import the documentation's prose or the Linux implementation as
replacement driver source. File contents and constants must be reviewed for
actual provenance, independently of their chosen license header.

Use `mke2fs`, `e2fsck`, and `debugfs` only as separately invoked host tools for
formatting and independent verification. Do not link libext2fs or other tool
implementation libraries into the kernel, embed their source in a driver, or
silently add their binaries to the OS image. Any future distribution of host
tools needs its own notices and source-obligation review.

## Implemented scoped source check

The provider's `tools/check_ext4_sources.py` checks the explicitly inventoried
`driver/fs/ext4` `.c`/`.cpp`/`.h` files against `SOURCE.json`: exact file membership,
SHA-256, one Apache-2.0 SPDX notice, and obvious copyleft notices in source
headers. It rejects symlinks, common alternate implementation/binary inputs in
that directory, and declarations of new external implementations or link
archives. The isolated `native.cpp`/`native.h` parser may only name `native.h`,
`stdint.h`, and `errno.h` in literal includes; include macros/imports and other
headers fail the check. Disposable negative fixtures exercise these rules.

Run it from the provider root with `python3 tools/check_ext4_sources.py`, or
from the parent with `python3 OpenXJ380/tools/check_ext4_sources.py --root OpenXJ380`.
Only update source hashes after reviewing the source change and its provenance.
The source declaration is an engineering review record, not evidence of
originality by itself.

**This scoped check is not a full compiler dependency or final kernel link
closure audit.** Existing platform headers, libraries, VFS, and build/runtime
support remain inside their existing review boundary. An empty declared archive
list does not inspect arbitrary linker commands or attest their entire input
closure. The broader checks below remain requirements for any future dependency
change or distribution; passing this script does not certify them or provide
legal clearance.

## Manual integration input check

The 2026-10-31 integration review compared both generated Ninja graphs with the
reviewed 18-file inventory (12 C++ implementations and six headers), including
the original native formatter. Each graph compiles exactly those twelve ext4
implementations and links each corresponding object once into its kernel. Each
also compiles/links the new original `driver/block_access.cpp` platform unit.
The shared, original Apache-2.0 installer code now has twelve implementation
units in `kernel/installer/`: `package.cpp`, `ext4_import.cpp`, `disk_layout.cpp`,
`ext4_verify.cpp`, `esp_verify.cpp`, `factory.cpp`, `boot_proof.cpp`,
`session_policy.cpp`, `session_runtime.cpp`, `source_lifetime.cpp`, `jobs.cpp`
and `service.cpp`. Original private RAM-tree staging, managed runtime callbacks,
atomic deep-tree publication and root-plan verification extend existing
first-party `tmpfs.cpp`, `vfs.cpp` and `installer_mode.cpp`; no third-party tmpfs
implementation or archive is introduced. The original
`kernel/task/kernel_worker.cpp` prepares kernel workers using the existing
allocator, task and scheduler interfaces without adding a task ABI field.
Original platform additions
`driver/block_source.cpp` and `kernel/memory/boot_memory.cpp` supply source
provenance and bounded UEFI memory-map parsing. The current inspected graphs
have 146 product object inputs and 135 provider object inputs; each also uses
the pre-existing `liballoc-x86_64.a`. These units and the standalone Python
encoder introduce no external implementation, runtime or archive dependency.
Their source review is separate from the 18-file ext4 directory inventory.

The installer handoff and device-path declarations express public UEFI protocol
ABI/format facts; no EDK2, Linux or filesystem implementation was imported.
The AHCI optical-capacity correction independently decodes SCSI big-endian
LastLBA fields, checks count/byte overflow and requests a nonzero response size.
Existing driver/loader code retains its prior licensing boundaries. The new
source sidecar does not expand `device_t` or introduce a runtime library.
New private ESP glue and preallocated scratch-arena hooks reuse existing FatFs.
Its implementation and Unicode source retain their original ChaN notices;
`ff.cpp` and `ffunicode.cpp` were not modified. Additions in the existing
`ffsystem.cpp` sample are identified as original Apache-2.0 port additions,
without relabeling the whole pre-existing sample file.
Neither static link adds another archive or library argument, and the linker
scripts contain no additional INPUT/GROUP/INCLUDE link inputs. Both generators
run the scoped source check before emitting the graph and require its preflight
for the ext4 compile and kernel link edges.

This records an inspected integration snapshot, not an automated dependency
closure attestation. Existing platform headers, runtime archives and unrelated
components retain their previous review boundaries. Future graph/source changes
require review again; the source checker alone does not repeat this link check.

### Managed root and native installer entry review, 2026-10-31

This stage adds independently authored Apache-2.0 managed RAM callbacks and
bootstrap publication/handoff code, the native installer service and fixed ABI,
kernel-worker preparation, user SDK marshaling, and the native installer UI
model/event path. New shared files carry Apache-2.0 notices. Changes within
existing VFS, process, syscall, graphics, loader and staging files retain those
files' existing licensing boundaries; this does not relabel either project as a
whole. No external implementation source, vendor subtree, generated upstream
code, runtime library or link archive was introduced for this stage. The RAM
backend is original code and does not import another tmpfs implementation.

The native service/ABI and SDK use only existing kernel/user interfaces and
fixed integer/layout declarations. The product graphics entry reuses its
existing graphics module and resources. It adds no new external module or
library dependency. The provider still has no product GUI implementation.
These files are outside `driver/fs/ext4/SOURCE.json`; their Apache notices and
integration review do not enlarge the scoped ext4 check's coverage.

A read-only snapshot of the current generated graphs resolved each explicit
kernel object to its primary source input and checked that the twelve ext4
implementations, installer service, kernel worker, VFS and RAM-root units each
appear once. Both graphs contain the original user SDK source edge. Each kernel
link has only the existing `liballoc-x86_64.a`, retains all five allocator wrap
flags, and uses a linker script with no extra INPUT/GROUP/INCLUDE directives.
The snapshot records graph/source/archive hashes in a local review artifact;
it does not recursively inspect compiler includes, archive members, dynamic
module dependencies or the whole repository's provenance. The existing ext4
source policy still checks its 18-file inventory only. Neither check is a
general license-compatibility certificate or legal clearance for the projects.

No Git refs, history, reflogs, remote objects or submodule history were changed
or removed in this stage. The historical GPL-related findings and unresolved
history scope below remain separate from this current-source integration
review; current Apache-2.0 additions do not resolve those historical questions.

### Large-volume arena and formatter review, 2026-10-31

The large-volume changes extend the existing original Apache-2.0 writer,
mapping, orphan, xattr and formatter units. Explicit caller-owned arenas replace
the fixed 128-group limit without adding a filesystem implementation, allocator
library, runtime or link archive. The pure writer/formatter code does not own
allocation or deallocation; existing first-party kernel adapters allocate and
retain the storage through their established success or quarantine lifetimes.
Importer/verifier and host-image changes remain separate first-party inputs
outside the scoped 18-file driver inventory. Their inclusion does not expand
the source check into a whole-kernel dependency or provenance audit.

Implementation inputs remain public format facts and independently generated
fixtures. The formatter's floating bitmap/inode-table locations, complete GDT
and sparse backup-super locations were checked against Linux tools; no Linux,
lwext4 or libext2fs implementation source was imported or translated. The host
image builder continues to invoke independent e2fsprogs processes rather than
link their libraries. New sparse test-device and reference-census harnesses are
original Apache-2.0 test code. They introduce no distributed tool binary or
production implementation dependency.

The writer bounds its geometry-specific arena at 256 MiB; this does not bound
all mount memory or guarantee allocation under pressure. The host-image profile
is conservatively limited to a 1 TiB ext4 root / 8192 groups. The native
formatter still serializes fewer than 2^32 blocks of 4 KiB, independently of
the writer's lower resource-admission bound. These engineering limits are not
license conclusions. Functional evidence is recorded in
`/tmp/xj380-large-formatter-validation.json` and
`/tmp/xj380-large-volume-host-validation-a5.json`: an actual 32 GiB plus 4 KiB
native format passed primary/backup Linux fsck; a separate 1 TiB sparse
Linux-formatted volume passed small native mutations. The latter does not
establish full native formatting or filling of a 1 TiB disk. Host fault models
do not certify real hardware power-loss behavior.

A separate read-only snapshot of both completed build graphs resolved 135
provider and 146 parent explicit kernel objects to their primary source inputs.
Each link contains the same twelve native ext4 units exactly once, only the
existing `liballoc-x86_64.a`, all five allocator wrappers, and no new linker-script
INPUT/GROUP/INCLUDE directive or response file. Source/archive/graph hashes are
recorded in `/tmp/xj380-large-volume-link-input-review.json`. This covers explicit
link arguments and primary edges, not recursive include or archive-member
provenance.

The existing Apache-2.0 source inventory and review-before-hash-update rule
remain in force. No external dependency is approved by this extension, and no
historical refs or objects are removed by it. Functional tests, source hashes
and SPDX checks do not establish originality, close the historical GPL review,
or constitute legal clearance for the closed parent or Apache-2.0 provider.

## Historical-reference review boundary

A separate read-only review found that the outer repository's old `code-cube`
branch and remote-tracking history still contain a `third_party/lwext4`
submodule gitlink to `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`, introduced by
`6e8aa23f283471b4e3bdb7222f627a7a184fb215`. The current HEAD does not contain
that integration. Git's `rev-list --all --objects` does not traverse submodule
gitlinks; missing vendor path labels in that output cannot prove complete
historical removal.

Only leading license comments and Git tree/change metadata were inspected in
this review. A historical file named `lwext4_permissive_stubs.c` actually declares
`LicenseRef-XJ380-Proprietary`; the other inspected glue files have no leading
license notice. Neither their names nor the absence of a GPL header establish
permissive rights or non-derivation. Those files were not imported into the new
implementation. No historical implementation body or external submodule source
was used for this review.

This does not establish which previously identified unpublished integration
commits were removed. Cleanup statements must identify that scope and cannot
be extended to all branches, remote histories, submodules, reflogs, unreachable
objects or backups. No existing historical refs were rewritten by this review;
the current implementation's source/build audit is a separate record.

## Dependency gate to enforce before release

The ext4 gate should fail closed and be applied to both Ninja generators and
both kernel link paths. The following checks are required; they are acceptance
criteria for the gate, not a claim that a future backend has already passed.

1. Maintain a reviewed manifest of every first-party ext4 source/header and
   every approved external input, with source provenance and selected SPDX
   expression. For external inputs, require an immutable revision, SHA-256
   inventory, complete license texts/notices, enabled features, and resolved
   dependency closure before the first import. The external list is currently
   empty. Do not regenerate approval automatically merely to accept a diff.
2. Check every implementation file against that inventory, including helpers,
   generated sources, headers, optional features, and build scripts. Missing or
   changed source origins and unreviewed licenses fail the gate. A directory
   name, wrapper license, or top-level manifest cannot override file notices.
3. Compare actual compile dependencies and final link inputs with the approved
   closure. Include archives and generated objects, not just files directly
   beneath the driver directory. Disallow unreviewed filesystem objects even if
   they are renamed or placed in an apparently first-party directory. Preserve
   the previously reviewed kernel baseline rather than blanket-approving it.
4. Run a supplemental header scan for copyleft notices or unexpected upstream
   attributions in new ext4 implementation files. A clean scan is only a signal;
   it does not establish originality, license compatibility, or completeness.
5. Verify license/provenance documents and attribution are staged where required
   and source bundles contain only approved material. Keep host-format tools
   outside the linked implementation closure. Test the gate with deliberately
   missing entries, unexpected sources, changed hashes, and disallowed notices.
6. Require a reviewer to examine provenance, license exceptions, and the actual
   source diff before changing the approved dependency set. Automated SPDX and
   hash checks must not be represented as a replacement for that review.

A build can satisfy this license boundary while its filesystem remains unsafe
or incomplete. Read-only, writes, journal durability, xattr semantics, and boot
readiness require the separate functional acceptance plan in `EXT4.md`.
