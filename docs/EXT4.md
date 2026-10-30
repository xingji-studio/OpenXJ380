# ext4 integration status and replacement plan

The kernel currently has **no ext4 driver**. The GPL-bearing prototype has been
removed. The OS build rejects `VDISK_FS=ext4` and `--fs ext4` before touching the
staging directory or output image. FAT32 image building remains available.

`tools/make_disk_image.py` in the core provider is an independently written host
formatter. Its ext4 path is retained for disposable format fixtures and future
driver development; it does not make an ext4 XJ380 root usable. It invokes host
programs as separate processes rather than linking their libraries into XJ380.
No ext4 image should be advertised as a working system until the driver passes
the acceptance criteria below.

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

Recommended direction: build a native freestanding C++ VFS driver in OpenXJ380,
with new first-party code under Apache-2.0. Reuse only separately audited
permissive components where they reduce the porting and maintenance cost.
An entire alternate runtime should not be added solely to hide missing
filesystem functionality. Do not start a port based only on its README claims.
Use the [on-disk format documentation](https://www.kernel.org/doc/html/latest/filesystems/ext4/)
and independent Linux-generated fixtures; do not copy Linux driver code.

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
   checks. Initially restrict writes to explicitly supported, clean, journal-free
   disposable images. Do not enable a writable system root at this stage.
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
