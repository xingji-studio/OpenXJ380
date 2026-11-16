# Fatal exception hooks

`kernel/wsod/wsod.cpp` declares two optional weak C++ hooks:

```cpp
void process_wsod(const char *error, X64_REGS *regs);
void process_wsod_without_regs(const char *error);
```

A consuming product can link ordinary strong definitions into its core kernel.
OpenXJ380 builds and boots without those definitions. A callback must not return
to normal execution. If missing or returning, the provider captures the fault,
stops other CPUs, writes an emergency serial diagnostic and permanently halts.
Fatal kernel page faults use the same entry. Existing recoverable user faults
and Linux synchronous signal handling keep their process-local behavior.

The callback should call `wsod_begin(regs)` immediately. Only the first CPU
succeeds; recursive and secondary entries must halt. `wsod_stop_other_cpus()`
broadcasts an NMI and waits for a bounded number of polls for initialized CPUs'
snapshots. Failure must prohibit raw storage access. NMI and double-fault stacks
are static, per CPU and independent of the assembly-fixed `PROCESSOR_INFO` layout.
No per-CPU preemption depth is changed by the fatal gate.

`wsod_cpu_states()` exposes the original register and FXSAVE64 snapshots and
bounded process/thread identity copies. `wsod_read_memory()` checks present page
table mappings without taking the normal page lock or triggering lazy allocation.
It cannot recover arbitrary physical memory corruption. Debug registers are not
read because DR7.GD can itself cause #DB. No-register calls explicitly lack an
original exception frame. Fatal code must never allocate, free, yield, take a
mutex, invoke ordinary VFS/GUI callbacks or wait on another task's spin lock.

`wsod_storage.h` provides preparation-only FAT/ext4 data-sector mapping and
preallocated fatal polling targets for AHCI SATA, NVMe and IDE with supported
512-byte geometry. The caller must retain and seal the ordinary file before
publishing the map, and authenticate its physical device's durable-flush support
in normal task context. Only the file's original preparing TCB/TID may initialize
it after pinning; sealing makes all VFS mutations fail until reboot. A retained
reference prevents unmount, and ext4 checks other open inode aliases before mapping.
The fatal writer is authorized only after every initialized CPU has stopped.
It never releases/reuses an interrupted command's DMA storage, even on timeout,
and never resumes the system after submission.

These interfaces do not add a standalone graphical crash screen, disk layout,
report naming policy or mandatory product implementation. Optional
`serial_crash_record(data, length)` receives bounded serial producer chunks outside
TX locks; it must not block, allocate, or reenter serial output.
