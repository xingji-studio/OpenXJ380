# Windows NTFS interoperability acceptance

This record covers the 2026-11-17 bounded P3 investigation. Normal Windows
round trips and recovery of a Windows-produced crash sample pass. The v5 native
dirty-WAL handoff now passes the complete Windows namespace/hash comparison,
including committed metadata before any home write. The environment and
remaining release boundaries below still apply.

## Environment and source protection

The user supplied existing installation media for read-only use. Windows 10
WinPE 10.0.19041.2006 runs from a private ISO copy in a new VMware Workstation
17.6.4 VM. The original installation ISO and its copy have SHA-256
`3c455d387431f6f760925908a4931f5d38edde397a66f1a5a0a47cf528677dbb`.
The installation ISO, existing VMs and physical disks are never test targets.
No source ISO, WIM or original VM is modified. The artifacts remain local;
Windows media and disk images are not redistributed with this repository.

Each run creates an independent 128 MiB raw GPT disk with a fresh disk GUID,
a 16 MiB Microsoft reserved partition at 4 MiB, and one Windows-formatted NTFS
data partition at 20 MiB. The tested volume is 113180672 bytes, with 512-byte
logical sectors, 4096-byte clusters/index pages and 1024-byte FILE records.
The VM has no NIC, host shares or physical-disk descriptor. The runner accepts
only regular disk image files and verifies input hashes after execution.

The original Windows agent additionally requires WinPE, the exact disk size,
one disk, the expected GPT GUID and partition bounds. It verifies volume label,
volume-to-disk mapping and a nonce marker before file operations. Windows
evidence uses fixed-size CRC/nonce/sequence records in the reserved 64 KiB..4 MiB
region outside every partition. That region is test instrumentation, not an
NTFS log or a feature of the shipped driver. The host reads only that region
while the VM is running. Gaps, wrapped evidence or incomplete manifests cannot
pass acceptance. The VNC console binds loopback only, uses an ephemeral password,
and its listener PID is checked against the new VM before sending keys.

The SDK/MSVC runtime and Windows APIs belong to the separate test agent. They
are never linked into the OS. All supervisor, agent and image-checking code is
original Apache-2.0 code. Existing Apache-2.0/MIT notices and GPL exclusions
continue to apply; this test introduces no new driver implementation dependency.

WinPE is Microsoft's deployment/recovery environment, with its Windows NTFS
driver and Win32 APIs. These results do not establish installed desktop Windows,
hardware validation, physical power-loss behavior or general NTFS compatibility.
See Microsoft's [WinPE description](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-intro?view=windows-11).

## Results and failures

| Case | Result |
|---|---|
| Windows format, seed files, normal shutdown, native reads | Complete namespace, sizes and SHA-256 match |
| Production core create, write beyond EOF, shrink/grow, move, Unicode rename, unlink/rmdir | Windows verifies contents and uppercase lookup; readonly `chkdsk` before/after returns 0 |
| Windows writes a new 4097-byte file after native changes | Native reader matches the complete Windows SHA-256 manifest |
| Actual XJ380 VFS via AHCI and NVMe on Windows-created copies | Basic operations/remount succeed; partition boundaries remain exact; backend writes are sector aligned |
| Kernel-written NVMe copy returned to Windows | Content/case checks and two readonly `chkdsk` checks pass; complete return-file hashes match |
| Host stops Windows during rolling create/delete/rename workload | Actual LFS 1.1 crash image captured at reported iteration 544 |
| Native recovery versus Windows recovery of independent copies of that crash | All 30 user entries, sizes and SHA-256 match; native-recovered result passes Windows readonly `chkdsk` |
| ASan/UBSan fault matrix on that actual crash image | 428 write/flush cuts and 428 recovery recuts pass, with volatile writes retained or lost |
| Torn native home write, recovered through XJ380 before Windows | Complete file/hash comparison and readonly `chkdsk` return 0 |
| v4 torn-home source attached directly to Windows | Historical failure: the expected volume was unavailable; no `chkdsk` acceptance was reached |
| v4 committed native WAL before home writes | Historical failure: Windows omitted `recovered.txt` despite readonly `chkdsk` returning 0 |
| v5 production committed native WAL before home writes | Complete six-entry namespace and SHA-256 match; readonly `chkdsk` returns 0 |
| v5 native restart/home/flush/cleanup failure samples | Eight Windows cases pass complete expected namespace/hash comparison, readonly `chkdsk` and native readback |
| ASan/UBSan native create and recovery fault matrix on the Windows seed | 406 write/flush cuts and 22 repeated-recovery cuts pass |
| Native recovery of the actual Windows crash interrupted after replacement publication or during its first home write | Windows recovers all 30 expected entries/hashes; readonly `chkdsk` and native readback pass in both cases |
| Kernel grows a real Windows-created MFT through 272 new files | AHCI/NVMe MFT initialized size grows from 262144 to 524288 bytes; Windows verifies all 285 returned entries/hashes and both readonly `chkdsk` checks return 0 |
| MFT growth interrupted halfway through its mirror or primary record-0 write | Windows and native readback match all 221 expected entries/hashes; readonly `chkdsk` returns 0 in both cases |

The Windows comparison excludes root system metadata names (`$*`) and
`System Volume Information`; it compares every other entry. It includes the
nonce marker, empty/resident/nonresident files, Unicode/surrogate names,
zero-filled growth gaps, uppercase lookups and the return file. A directory or
file omitted by either reader fails the comparison. Passing `chkdsk` without
the expected committed namespace does not satisfy journal acceptance.

Acceptance runs use `chkdsk R:` without repair switches, and require exit 0.
A separate diagnostic clone was repaired to identify the filename namespace
failure; its report is explicitly `passed: false, diagnostic_only: true` and
cannot be used as an acceptance manifest. Microsoft's [command documentation](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk)
distinguishes status-only checks from repair switches.

Failures found and fixed from these samples:

- A valid LFS NTFS client can have sequence number zero. It is checked against
  each record, without imposing the nonzero rule for MFT references.
- A newly formatted Windows root can contain a passive 56-byte resident
  `$TXF_DATA` initializer. Only the observed empty-state signature is allowed,
  and its complete attribute must remain identical. Active, unknown or logged
  changes remain unsupported; TxF transactions are not implemented.
- A single filename without its DOS alias uses namespace 0. Creating it as
  namespace 1 caused real Windows filename/index errors. Namespace 0 lookup
  follows the directory case policy, including ordinary insensitive lookup.
- An ordinary Windows transaction can terminate with a compensation-shaped
  FORGET and zero undo-next, without a separate COMMIT. Recovery now accepts
  that terminal form while refusing prepared or nonterminal unresolved outcomes
  before writes. The updated core passed 71 NTFS regression tests, the pinned
  MIT journal oracle, and AHCI/NVMe 4Kn kernel tests combining this form with MFT
  attribute-list growth (35879 writes, 1380 flushes).
- v5 control records retain the first unused LCN slot (40-byte minimum header),
  mark absent redo/undo buffers with the LFS zero-data flags, and keep the
  compensation-undo distinction. MST block hints, the preceding-global OPEN
  LSN, the first transaction link and terminal undo-next fields follow the
  observed Windows record conventions. COMMIT/FORGET and the original durable
  publication order remain in use.
- Once the padded native profile is recognized, a broken COMMIT or chain is
  refused by its strict reader. It cannot fall through to the general Windows
  parser and be reinterpreted as an ordinary terminal-FORGET transaction. A
  regression reconstructs v4 control records and checks successful, idempotent
  recovery; malformed native commits still perform zero writes and flushes.

## Dirty-log diagnosis

`trace-manifest` starts a bounded NTFS ETW session before publishing a fixture's
partition table to Windows. Its guest first authenticates an empty owned GPT
disk, copies the readonly fixture from its test CD, flushes it, then requests a
partition rescan. This avoids missing recovery diagnostics during boot. The ETL
is exported into the owned disk's reserved 2..4 MiB region with a length, nonce
and CRC; JSON evidence is kept below that region. Source fixtures and media are
hashed again after the run. The phase is always diagnostic and cannot pass
acceptance, even when its content checks succeed.

The same trace pipeline recovers the original Windows crash sample without a
corruption event. Native-log failures produce NTFS corruption events; public
function names locate successive failures in `ReadRestartTable`, `AnalysisPass`
and `RedoPass`. Original resident-value and whole-FILE probes isolated control
header and empty-buffer flags. A COMMIT without its empty-redo flag was rejected;
the same transaction with the flag passes. COMMIT itself is not excluded from
the v5 producer. Corrected full-transaction probes replay the expected create
with no corruption event (state 0), then the actual production implementation
passes the separate acceptance runner. Diagnostic reports remain diagnostic;
only complete content/namespace and consistency gates establish acceptance.

Only ETW data, PE/CodeView identification metadata and public function names
are used for that diagnosis. No Windows executable instructions, source code,
private types or source lines are inspected or ported. The public PDB remains
a local diagnostic artifact under Microsoft's symbol-server terms; it is not
vendored or included in the image. See the diagnostic reference boundary in
`driver/fs/ntfs/PROVENANCE.md`.

## Reproduction

From the parent XJ380 root, `tests/run_ntfs_windows.py` runs individual phases
on fresh owned directories. `--phase seed` creates and shuts down a Windows
volume. Later phases require its nonce and a regular `--source` disk, which is
copied before use. `native-core` and `native-vfs` verify known driver mutations;
`crash` hard-stops the owned VM during the metadata workload; `manifest` runs
Windows recovery and exports the complete namespace/hashes. The VMware backend
currently authenticates only 512-byte logical sectors. The QEMU WinPE backend
has not booted this media successfully and is not counted as acceptance.

`tests/run_ntfs_windows_acceptance.py` compiles the actual production core and
fault adapter under ASan/UBSan, runs Windows/core round trips, compares recovery
with Windows, cuts every recovery write/flush and checks native WAL handoff.
It records overall failure until all expected comparisons pass. Optional
`--qemu-vfs` adds the kernel AHCI/NVMe round trip. It needs a local licensed
Windows ISO, MSVC x64 toolchain, VMware Workstation/vmrun, PowerShell, xorriso
and the existing project build/QEMU tools. For example:

```sh
python3 tests/run_ntfs_windows_acceptance.py \
  --media /mnt/c/path/to/private-Windows-copy.iso \
  --work /mnt/c/path/to/new-owned-acceptance \
  --backend vmware --qemu-vfs
```

`--seed-case` can reuse a successful Windows seed case as an immutable source.
`--native-wal-only` reproduces the native-WAL gate without rerunning
the full Windows crash workload. Its report fails when a committed entry is missing.
Add `--native-faults` to derive eight representative cuts from the actual I/O
trace: the first/second dirty restart, first FILE/INDX/last home write, lost or
retained writes at the home flush, and the first clean restart. Interrupted
writes retain half the request. Before a complete dirty restart is durable,
the first torn-restart case must retain the old namespace; the remaining cases
must recover the committed namespace. Each Windows result also returns through
the native reader. The runner additionally cuts every native write/flush and
recovery recut under ASan/UBSan. It records the implementation source hashes.
It never resumes or overwrites a previous artifact directory. The normal
non-Windows regression suite requires no Microsoft media or SDK.

`tests/ntfs_windows_trace.py CASE` verifies and decodes a `trace-manifest` case
into compact NTFS diagnostics. `tests/run_ntfs_qemu.py --sector-size 4096
--attribute-list --windows-terminal-forget --mft-growth --boot-image IMAGE`
exercises the terminal-FORGET fixture through both real kernel storage paths.
Synthetic fixtures, diagnostic phases and public-symbol lookup are not substitutes
for passing the Windows content, namespace and readonly consistency gates.

`tests/ntfs_windows_log_probe.py --seed-case CASE --work NEW` preserves the
original minimal reproductions without distributing a Windows image. Its
diagnostic options isolate full-FILE updates, COMMIT, checkpoint tables, OPEN
fields and empty-data flags. Every output is a new owned copy.

`tests/run_ntfs_windows_recovery_handoff.py` additionally uses an owned Windows
crash case and its independent successful Windows recovery oracle. It derives
the recovery publication boundary from actual I/O, interrupts native recovery
after both replacement restarts or halfway through its first home write, then
hands fresh copies to Windows. The complete oracle namespace, sizes and hashes,
readonly `chkdsk`, native readback and immutable-source hashes are required.
This checks interrupted recovery as well as interrupted ordinary mutation.
With `--seed-case` instead of `--crash-case/--oracle-case`, the same tool derives
the first real MFT-growth mirror/primary writes and cuts each halfway through.
Native recovery provides the expected committed tree, which Windows must match
exactly. Hosted bulk image workloads use owned Linux temporary files; bounded
offset-based copies retry transient DrvFS ENOMEM/short writes before any result
is accepted. Infrastructure-only failures remain failed diagnostic attempts.

The QEMU Windows growth probe derives its file count from the seed's initialized
MFT capacity. A fixed 80-file synthetic probe did not exhaust Windows' 256
initialized slots and was rejected as insufficient evidence. The 272-file case
does expand the MFT. Entry batches fit the reserved control area while preserving
CRC/nonce and complete record-sequence checks; batching cannot contain status
events or hide missing records.

## Remaining release acceptance

The v5 native dirty-log handoff passes the bounded Windows seed and selected
failure cases above. This does not establish arbitrary Windows attachment after
every failure on every NTFS volume. Unsupported states still fail closed, and
all mounts remain subject to the documented feature and resource bounds.

More independent Windows crash points and workloads, installed Windows 10/11,
actual Windows-produced LFS 2.0/4Kn logs, Windows-side checks after MFT
ATTRIBUTE_LIST growth, larger representative volumes, active ACL/USN features
and physical-device crash behavior remain outside the present evidence. Unknown
operations and unresolved outcomes must continue to fail closed. The bounded
resource/feature limits in `NTFS_WRITE_DESIGN.md` still apply.

No patent clearance is inferred from these tests or from the retained licenses.
The Microsoft Open Specifications Promise applies to its listed specifications
and claims; an overall NTFS on-disk grant has not been established. The release
review boundary remains in `driver/fs/ntfs/PROVENANCE.md` and Microsoft's
[OSP notice](https://learn.microsoft.com/en-us/openspecs/dev_center/ms-devcentlp/1c24c7c8-28b0-4ce1-a47d-95fe1ff504bc).
