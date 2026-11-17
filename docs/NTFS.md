# NTFS data-volume prototype (P0/P1/P2/P3)

The built-in `ntfsro` driver reads a bounded NTFS 3.1 data-volume profile.
Both XJ380 and standalone OpenXJ380 register it after the existing filesystems.
Partition automount recognizes supported volumes; an explicit kernel VFS source
is `ntfsro:/dev/<partition>`. A normal block-device source also probes read-only.
NTFS is not a supported system root or installer target.

P3 adds explicit `ntfsrw:/dev/<partition>` with MFT ATTRIBUTE_LIST/extension/bitmap growth and 4Kn metadata writes, bounded native and foreign LFS 1.1/2.0 recovery,
cluster allocation, file creation/deletion, directory creation/removal, rename,
resident/nonresident resizing and file writes with EOF extension. Automatic
probing and `ntfsro:` remain read-only. The metadata profile, limits, fault
contract and outstanding Windows interoperability acceptance are described in
[NTFS_WRITE_DESIGN.md](NTFS_WRITE_DESIGN.md).

## P2 supported profile

- Resident/nonresident unnamed `$DATA`, empty files, 64-bit random offsets,
  fragmented mapping pairs, explicit sparse holes and valid-data-length zero-fill.
- Resident/nonresident `$ATTRIBUTE_LIST` in the base record. Stream fragments,
  including `$MFT`, `$I30` and `$UpCase`, can reside in extension records. Sequence
  numbers, extension backreferences, attribute identity and contiguous VCN coverage
  are checked. The first extent owns stream sizes; continuation size fields must
  be zero or agree. Relative LCN decoding restarts for every fragment.
- Resident `$I30` indexes and bounded external INDX trees with bitmap checks.
  UTF-16 names, including valid surrogate pairs, are exposed as UTF-8.
  Enumeration preserves stored spelling. A DOS alias is suppressed only when
  the same reference also has a long name in that directory; DOS-only names remain.
- Lookup uses the volume's validated 128 KiB `$UpCase` table. Exact spelling wins
  over a folded match. Namespace 0 names follow the directory case policy too;
  directories carrying the `$STANDARD_INFORMATION` case-sensitive flag require exact case. Unicode is
  neither normalized nor interpreted through a host locale.
- Case variants and unambiguous DOS aliases resolve to the existing cached VFS
  node. Ambiguous folded matches fail with `EIO`. A DOS alias with multiple
  possible long hardlink names in the directory fails with `EOPNOTSUPP`.
- NTFS timestamps are converted for display. Local policy is uid/gid 0,
  directories 0555 and files 0444. Windows ACLs/WSL ownership are not translated.

## Clean-volume mounting

P2 accepts NTFS 3.1 with no dirty, recovery, repair or upgrade request. Persistent
volume policy flags in mask `0x3c80` are allowed; other unknown flags are refused.
An entirely uninitialized (all `0xff`) `$LogFile` is supported. Initialized logs
require two valid, compatible `RSTR` restart pages using LFS 1.1 or 2.0. USA
trailers, restart geometry, client lists and LSN bounds are validated. The newest
LSN, then restart revision, selects the effective clean state. A stale clean
copy cannot override a newer dirty copy. A clean flag or an empty active client
list is required. Nothing replays or modifies the journal.

Both restart copies must validate. Torn redundancy, mixed versions, unsupported
LFS versions and ambiguous ordering are refused. This conservative mount policy
is not a recovery implementation or a whole-volume consistency checker.

An absent root `hiberfil.sys`, an empty file or an entirely zero first 4096 bytes
is accepted. Nonzero headers, including active hibernation/Fast Startup states,
are refused. Multiple case variants naming different hibernation records are
refused. The driver never clears the header or dirty state. A Windows volume
must be fully shut down and detached from other writers before use; live shared
access and snapshots of running Windows volumes are outside this profile.

## Limits and unsupported features

Limits fail explicitly instead of returning a truncated directory or guessed data:

| Item | Bound |
|---|---|
| Logical sector size | 512..4096 bytes, power of two |
| MFT record size | 512..4096 bytes |
| Index record size | At most 16384 bytes, at least one cluster |
| Mapping runs | 1024 per assembled stream |
| Attribute list | 64 KiB and 1024 entries |
| Directory index | 65536 keys including aliases, 4096 external nodes, 16 levels including root |
| LFS system/log page | 512..65536 bytes, power of two |
| LFS clients | 64 |
| Fresh all-`0xff` log scan | 64 MiB; initialized logs require only restart-page reads |

An attribute list must itself be fully described by one attribute in the base
record; that attribute may contain multiple mapping pairs. Split attribute-list
streams and nested lists are unsupported. `$MFT` bootstrap follows fragments in
VCN order; each extension record must be reachable through the already known
MFT prefix. No physical location is guessed to break a bootstrap dependency.
The workspace and volume tables use approximately 1 MiB per mount; large scratch
data is allocated on the heap, not the kernel stack.

Compression, EFS, WOF, offline/recall data, reparse targets, LFS 3.0/DAX, unsupported journal
operations and unresolved prepared transactions remain unsupported. Metadata mutations use
the explicit P3 geometry, bootstrap, transaction and resource bounds documented separately. Named data streams are not exposed.
Reserved records 0..15 are hidden by the VFS adapter. Unsupported reads return
`EOPNOTSUPP`; dirty volumes return `EUCLEAN`, malformed metadata and media
failures return `EIO`. Errors never become successful EOF.

## Read-only and lifetime guarantees

The ordinary mount accepts only a `Reader`. Write and flush callbacks are accepted
only through a separate experimental activation, and the kernel requires an
explicit `ntfsrw:` source and an authenticated physical flush capability.
For `ntfsro`, the VFS read-only property blocks mutation and callbacks independently
return `EROFS`. The explicit metadata session permits the bounded operations
described in NTFS_WRITE_DESIGN.md. Mapping and arbitrary backend ioctls are
unsupported. The experimental writer supports only the native block-sync request
(legacy `IOBLKSYNC`) and uses the existing real device flush path. No access-time
updates, dirty-bit changes or repair writes occur on Reader-only mounts.

Device references, generations and block-access leases are retained until
checked unmount. A sleepable operation mutex protects workspaces and
private-before-public directory construction. Busy VFS handles prevent unmount.
The optional name resolver runs outside spin locks while managed traversals are
pinned, then acquires the existing node through the normal reference path.
Existing vnode/callback ABI layouts are unchanged.

An abandoned mutex owner or uncertain device I/O poisons the affected state.
Objects and leases are retained until reboot rather than reused after possibly
interrupted DMA/publication. Process-exit hooks run before FD cleanup.

## Verification and remaining validation

From the parent XJ380 root:

```sh
python3 OpenXJ380/tools/check_ntfs_sources.py
python3 -m unittest discover -s tests -p 'test_*ntfs*.py' -v
python3 -m unittest discover -s tests -p 'test_vfs_readonly.py' -v
python3 -m unittest discover -s tests -p 'test_provider_staging.py' -v
python3 tools/gen_ninja.py --out build.ninja
./ninja -f build.ninja all -j8
python3 tests/run_ntfs_qemu.py
python3 tests/run_ntfs_qemu.py --experimental-write
python3 tests/run_ntfs_qemu.py --metadata-write
python3 tests/run_ntfs_qemu.py --metadata-recovery
python3 tests/run_ntfs_qemu.py --windows-recovery --mft-growth
python3 tests/run_ntfs_qemu.py --sector-size 4096 --attribute-list --windows-recovery --mft-growth
python3 tests/run_ntfs_journal_oracle.py --windows-recovery
python3 tests/run_ntfs_journal_oracle.py --sector-size 4096 --attribute-list --mft-growth --windows-recovery
```

Host tests compile the production core under ASan/UBSan. Fixtures include fresh
`mkntfs` volumes, large external directory indexes, constructed clean LFS 1.1/2.0
logs, stale clean versus newer dirty restart pages, resident/nonresident attribute
lists, fragmented MFT bootstrap, 600 mapping runs, Unicode/case-sensitive lookup
and offsets above 4 GiB. Malformed metadata and hibernation states are exercised.
Host filesystem utilities run as separate processes on disposable images.
P3 adds MFT spare-space/physical growth, resident/nonresident bitmap growth,
ATTRIBUTE_LIST/extension growth and actual 4Kn metadata writes,
packed and wrapping LFS 1.1/2.0 logs, checkpoints, tail copies, active/committed/
compensation outcomes, refused prepared/unknown operations and full write/flush
fault matrices with repeated recovery. Constructed foreign logs and independent
MIT replay check format/results. Windows-created clean volumes and one actual
Windows LFS 1.1 crash sample have also passed bounded WinPE cross-checks; see
[NTFS_WINDOWS_ACCEPTANCE.md](NTFS_WINDOWS_ACCEPTANCE.md) for evidence and remaining gaps.

The QEMU probe uses AHCI/NVMe partitions. Its NVMe fixture combines clean
initialized logs, MFT/data extension records, Unicode folding, a DOS alias and
a case-sensitive directory. It checks VFS node identity, rejected writes, busy
unmount/remount, zero data-device write/flush requests and unchanged complete
data-disk hashes. The AHCI fixture is independently made by `mkntfs`.
With `--sector-size 4096`, the NVMe device and its GPT use actual 4 KiB logical
sectors; AHCI stays at 512 bytes because QEMU 6.2's IDE model requires that size.
The metadata probe checks every backend write against the reported sector size.

A Windows 10 WinPE-created NTFS 3.1 volume now passes native readback, normal
write round trips, Unicode/case lookup and independent Windows file hashes.
This is a bounded deployment/recovery environment test, not general Windows
desktop, real 4Kn or hardware power-loss acceptance. The v5 native dirty-WAL
handoff passes the complete Windows namespace/hash comparison before home
writes and at selected restart/home/flush failure cuts. A successful `chkdsk`
alone is never counted as committed journal replay.

## Licensing

P3 metadata transactions additionally retain the reviewed MIT DiscUtils journal
notices; no C# runtime or archive is linked. See `driver/fs/ntfs/PROVENANCE.md`, `SOURCE.json` and `REFERENCES.json` for the
immutable upstream revision, adapted fragments, format references, changes and
review limits. Binary staging retains those records and the unmodified upstream
LICENSE/NOTICE under `/system/licenses/native-ntfs`. Only the inventoried Apache-2.0/MIT driver sources are newly linked; no
external archive/runtime or GPL implementation is added. Copyright permission does not establish
Microsoft patent clearance; jurisdiction/claim review remains outstanding.
