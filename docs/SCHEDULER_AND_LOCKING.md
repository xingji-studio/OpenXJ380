# Scheduler, allocator, and GUI locking invariants

This document records failure boundaries that are easy to break during an
otherwise local refactor. Read it before changing scheduler state, allocator
wrappers, exec/image publication, lock queues, or sheet composition.

## Scheduler state has two different scopes

The scheduler has a global boot/fatal gate and a per-CPU runtime nesting
counter. They solve different problems:

- `scheduler_start()` opens scheduling after boot has initialized all CPUs and
  queues. `scheduler_stop()` closes scheduling for fatal/panic handling.
- `disable_scheduler()` increments only the current CPU's
  `scheduler_disable_depth`; `enable_scheduler()` decrements it without
  underflow.
- `scheduler_is_enabled()` is true only when the global gate is open and the
  current CPU's depth is zero.
- `scheduler_global_enabled()` reads only the global gate. A service that has
  already started may use it to detect fatal shutdown; a nonzero local depth
  must never be mistaken for system-wide shutdown.

Never use `enable_scheduler()` to start the system scheduler. Never replace a
save/restore pair with a naked enable: the caller may have entered with a
nonzero depth. A function that changes the depth must restore its entry value
on every error path and before a non-returning user-mode handoff.

`PROCESSOR_INFO.current_task`, `syscall_user_rsp`, and `syscall_user_rax` have
fixed offsets consumed by `kernel/intr/handler.S`. The compile-time assertions
in `kernel/task/scheduler.cpp` are intentional ABI checks. New per-CPU state
belongs after those assembly fields unless every assembly offset is reviewed
and updated together.

Each AP must restore the BSP's MTRR/PAT snapshot and clear CR0.CD/NW before
LAPIC calibration or runnable task publication. INIT leaves the AP's caches
disabled; the trampoline and FPU setup do not enable them. `mtrr_save()` records
the exact BSP default memory type, enable bits and optional fixed/PAT state.
`mtrr_restore()` runs only during AP startup with IRQs disabled, flushes cache
and TLB state (including global translations), restores supported memory-type
MSRs, and enables caching while preserving other CR0/CR4 bits. Do not use it as
a live runtime MTRR reconfiguration API.

`tests/test_cpu_cache_init.py` checks the production restore sequence against
traced register adapters. `python3 tests/run_cpu_cache_qemu.py` checks actual
CR0 on all four CPUs after application startup; `--accel kvm` uses normal KVM
device permissions. The runner changes only a temporary image. TCG checks
architectural state but does not establish real cache-performance gains. KVM
handling of guest cache controls also varies by host/backend configuration;
do not infer a measured speedup solely from register values. See the
[Intel SDM](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf)
cache-control/MTRR sections and the
[KVM memory-type policy](https://github.com/torvalds/linux/blob/v6.8/arch/x86/kvm/vmx/vmx.c#L7109).

Terminal status alone does not make a task or process reclaimable. The timer
publishes its selected task under the CPU queue lock before releasing that
lock. When it publishes a new `current_task`, it retains the previous stack
owner until the next interrupt entry proves that `iretq` completed. Private
reference arrays preserve the fixed `PROCESSOR_INFO` layout and use CPU array
indices rather than ACPI IDs. `scheduler_task_referenced()` and
`scheduler_process_referenced()` include all three references. Mark a task
terminal before checking, and remove it from its scheduler queue before free.
Never call these queries while holding any CPU scheduler queue lock. Queries
perform bounded scans without allocation, yielding or I/O.

Timed sleep publishes its deadline and `WAIT` under the task's CPU scheduler
queue lock. Remote wake rechecks `WAIT` under that same lock before clearing
the deadline and preparing wakeup credit; an observation of an older sleep
must not clear a newer deadline. Published runnable tasks do not migrate, so
their `cpu_id` remains the queue-array index until terminal removal. Wake,
timer expiry and the sleep return path use CAS to preserve a concurrent
`DEATH`/`OUT` publication. Compute the sleep time before locking; these short
publication sections must not read clocks, log, yield or allocate. The expiry
helper already runs under the queue lock and must not acquire it recursively.
`tests/test_scheduler_sleep_wake.py` covers the delayed-old-wake interleaving
and terminal publication at sleep, expiry and the final resume CAS.

EEVDF runtime is measured from actual elapsed nanoseconds on each CPU, including
voluntary blocking before the next hardware tick. A task publishes `WAIT` before
entering the software timer interrupt, so status alone cannot decide whether its
last CPU burst needs accounting. The timer charges its pinned current task even
after `WAIT`, `DEATH` or `OUT` publication. A private per-CPU anchor advances once
per accounted interval; actual handoff charges residual scheduler work before
starting the selected task's interval. Resuming a task never charges its off-CPU
sleep. Same-task publication and stale clock samples must not reset or rewind
the anchor. Read the clock before acquiring a scheduler queue lock; charge TCB
vruntime under that lock to serialize remote wake placement. No TCB or
`PROCESSOR_INFO` field or assembly offset is added for this accounting.

Wake placement caps old sleeper lag with `max(old_vruntime, queue_base - credit)`;
it must never erase consumed CPU time. A remote wake uses the target queue's
non-idle runnable current task, or its minimum runnable vruntime, rather than the
calling CPU's unrelated vruntime domain. CPU accounting and placement retain
terminal-state CAS rules. The 250Hz hardware timer and four-tick switch policy
remain unchanged; no task class receives an audio-specific priority or slice.
`tests/test_scheduler_fairness.py` executes the production timer and selection
with sixteen sub-tick sleepers competing against an ordinary four-ms-slice
producer. An old-policy control reproduces starvation; the fixed policy has
bounded dispatch. It also checks exact interval accounting, residual switch
work, sleep exclusion, clock anomalies, remote queue domains and saturation.

LAPIC timer calibration preserves the 250Hz target. Each endpoint brackets a
current-count read with HPET reads; subtract two observed counts from the same
continuous one-shot interval and normalize by their actual midpoint elapsed
time. Never assume the minimum wait was the full calibration interval: MMIO
latency before/after HPET samples previously produced different timer periods
on identical CPUs. Three bounded 10ms samples select the narrowest relative
brackets; combined uncertainty above 10% is rejected. Clock/counter failures
retain the existing finite fallback. `tests/test_apic_calibration.py` covers
delayed endpoints, overshoot, non-QEMU frequencies and failed clocks.

Exception, IRQ and syscall entry must preserve x87/MMX/MXCSR/XMM state before
calling C: optimized kernel helpers, including page clearing for a lazy user
fault, use SSE. Each nesting level owns a separate 16-byte-aligned 512-byte
FXSAVE64 image on its kernel stack; the existing GPR frame offsets stay fixed.
The timer publishes its entry image to the interrupted task before scheduling,
then restores the selected task's image only after all C helpers return. A
declined switch restores the private entry image. Do not move FPU save/restore
back into the middle of `change_proccess`, or share an exception image with a
TCB or per-CPU scratch buffer: C code and nested entries can overwrite it.

## Lock and allocator rules

liballoc's internal lock does not mask interrupts. If an interrupt arrives
while a CPU owns that lock and the interrupt path allocates or frees memory,
the same CPU can spin forever waiting for the interrupted owner. Kernel
allocation entry points therefore use IRQ-saving wrappers in
`kernel/memory/heap.cpp`; the kernel link must retain `--wrap=malloc`,
`--wrap=calloc`, `--wrap=free`, `--wrap=realloc`, and `--wrap=aligned_alloc`.
The root and `OpenXJ380` Ninja generators both produce kernel link rules.
`__real_*` calls in the wrapper implementation are the deliberate raw-liballoc
entry points. Do not add bypasses elsewhere. `calloc` is currently exported as
a normal symbol; its implementation must continue to route the underlying
allocation through wrapped `malloc`. If changing that ABI, preserve overflow
checking as well as IRQ safety.

Never call malloc/free, VFS, image decoding, rendering, sleep/yield, or a
blocking mutex while holding a spin lock. Never do blocking work with scheduler
preemption disabled. If a protected operation needs a large copy or allocation,
capture/pin the required state, release the spin lock, do the work, then
validate the generation/lifetime before publishing the result.

Mmap address selection and VMA publication, munmap VMA/lazy/PTE teardown, brk
and mprotect use the same preemptible memory-operation mutex. Range search reads
the mutable VMA list and cursor, so it belongs inside that scope as well as
insertion/removal. Descriptor acquisition precedes the mutex, and all errors
release it before FD-operation destruction. Never replace this blocking scope
with a spin lock: VMA allocation, lazy metadata and frame reclamation may allocate
or free memory. Concurrent TLS callback mappings and joins exercise this boundary.
The gate lives with lazy allocation and is recursive, covering reservation
lookup/split/merge/removal as well. User-copy or VFS work can fault from an
existing memory-operation scope. A not-present user fault may wait/allocate
only with inherited preemption depth zero; restore that depth before the slow
path and close IRQs before exception return. Its private exception/FPU frame
must remain intact across scheduling. Initialize a new frame through HHDM
before publishing a present user PTE, and recheck pages another CPU already
faulted in. Protection faults must not enter this allocation path.

## Runtime serial transport

Normal serial output formats into a private per-call writer and publishes
bounded chunks to static FIFO records. The queue lock protects metadata and
short copies only; never format, poll UART, render, notify observers, allocate
or wait while holding it. Only the permanent consumer accesses UART at runtime,
outside locks, with bounded FIFO batches and cooperative waits. IRQ, nested
preemption and consumer-reentrant producers must never wait for queue space.
Log acceptance is decided when a record is enqueued. Keep prompt state and
console/observer delivery in FIFO record order; raw output retains its original
log-filter behavior.

The separate serial owner-lifetime lock pins an incomplete record's TCB during
recovery. The consumer releases the serial queue lock before checking scheduler
references, then revalidates the record generation before closing it. Producers
never acquire the lifetime lock. `serial_task_quiescent_exit()` takes that same
lifetime lock before a TCB is freed, outside CPU scheduler queue locks. A dead
task may still execute a syscall: terminal status without scheduler quiescence
is insufficient. The worker also recovers abandoned heads so cleanup logging
cannot block a reaper before it reaches the before-free hook.

`scheduler_stop()` may call the weak serial panic hook only to publish an atomic
emergency flag. It must not acquire a lock, wait, allocate or perform I/O. This
explicit notification covers fatal shutdown before the consumer first runs.
Emergency output bypasses unfinished FIFO ownership and uses bounded raw UART;
it must not call console/observer callbacks that the interrupted context may
already own. An ordinary local preemption depth is never a panic transition.
`tests/test_serial_transport.py` exercises the actual transport and formatter
under ASan/UBSan, including these startup, shutdown and lifetime boundaries.

## Recovering abandoned UI locks

`mutex_lock_recoverable()` is opt-in for state that can be rebuilt, currently
image decoding, task-dock painting, and the input-method service. Ordinary
`mutex_lock()` retains its original semantics. A terminal thread may still be
executing a kernel call on another CPU, so recovery checks both DEATH/OUT status
and absence from the scheduler's current, selected and previous-stack
references. The mutex metadata spin lock pins
the owner pointer during this check: each participating subsystem must retain
its process-exit hook, clearing the lock before its owner's TCB is freed.
Never retain `mutex_get_owner()` outside that lifetime boundary for recovery.
The CPU scan is bounded; yielding and rebuilding happen after unlocking.
Recovery transfers the mutex directly to the recovering waiter while its
metadata lock is held. Publishing an unlocked mutex first would allow another
waiter to enter without learning that protected state needs recovery.

Ext4 uses this mechanism to fail closed, rather than rebuild an interrupted
in-memory transaction. An abandoned operation permanently rejects ext4 I/O
and new mounts until reboot, marks existing mounts failed and quarantines the
writer/device buffers. It never clean-closes or checkpoints that session.
`ext4fs_quiescent_exit()` must run before descriptor cleanup and before a
terminal owner's TCB is freed, so no mutex retains a dangling owner pointer.

Ordinary file syscalls hold kernel-private heap operation tickets while using
an FD. Close removes the descriptor immediately; its handle and VFS reference
survive until the final ticket is released. Ticket owners are reclaimed only
after the scheduler reference scan proves quiescence. Process exit drains FDs
from task context in the reaper, outside spin locks and scheduler-disabled
sections. PCB collection and waitpid claims must retain their private lifetime
pins while any cleanup or user-memory copy is in progress.

Known limit: forced termination of a waiter after it claims exclusive PCB
collection can retain that partially collected child until reboot. Collection
is not a resumable transaction, so another task must not blindly take over its
pin and repeat frees. This conservative retention is distinct from normal
waitpid/exit cleanup and from journal replay after reboot.
The input-method service reloads its verified settings journal and revokes the
old engine session when a caller dies while holding its service mutex.
Learning storage uses its own recoverable mutex with the same process-exit hook.
The service mutex must be released before storage/VFS operations. The storage
transaction may briefly acquire the service mutex to revalidate identity, so
never acquire the storage mutex while holding the service mutex. The async
save queue spin lock protects job publication/status only; snapshot allocation,
free, thread creation and VFS work happen after releasing it. Delayed writes
must check the policy generation under the storage mutex before writing.

The product window lifetime gate pins `WINDOWLS`, its controls and its sheet
across preemptible GUI calls. Only the same non-null TCB may acquire it
recursively; inner releases do not publish an unlocked gate or reset its hold
timer. Recovery and process-exit cleanup require a terminal owner and absence
from all scheduler current, selected and previous-stack references. The short
metadata spin lock protects ownership only: allocation, VFS, painting, logging
and yielding happen after unlocking. Keep the lock order window gate before
toast/IME state locks; toast desktop-presence checks take a short window
snapshot before taking the toast mutex.

GUI input, animation completion and notification actions share a private
asynchronous message publication queue. Publication copies a record and pins
the original descriptor, then transfers its operation ticket to kernel
ownership (`owner == NULL`) while holding the queue metadata lock. Thread-exit
cleanup must skip those tickets; the permanent delivery worker releases them.
Queued state retains no PCB or TCB pointer and never resolves a numeric FD
again. Descriptor retirement cancels pending delivery; private message pipe
endpoints use `O_CLOEXEC` so old callbacks do not survive exec.

Each original pipe has one FIFO. Only the delivery worker writes these GUI
records, using an all-or-nothing nonblocking packet write outside the window
gate. It rotates among pipes with a fixed work budget; one full destination
cannot block another application. Pipe spin locks cover only packet copies and
wait-list detachment. Waking detached waiters verifies numeric task identity
under process/thread list locks before dereferencing a TCB; freeing nodes
occurs after unlocking. Wait/wake status publication must not replace a
concurrently published terminal state.

Pending GUI delivery is bounded to 4096 records per pipe and 32768 globally.
At capacity the kernel returns `-EAGAIN` or `-ENOBUFS` and rejects new records
while retaining the existing FIFO; legacy void entry points receive a bounded
serial diagnostic. Allocation failure returns `-ENOMEM`. These bounds cannot
guarantee lossless delivery to an application that permanently stops reading;
never wait for pipe capacity while holding the window gate or silently grow
an unbounded queue to conceal that condition.

## Exec and IPC publication

Native process creation prepares a complete PCB before publishing it in the
process/family lists. A private `ProcessPreparation` ticket pins that PCB and
its page directory through ELF loading and the first thread's publication.
The same TCB may recurse; another preparer waits outside spin locks. Collection
and exec publication must check these tickets. Check the cleanup claim again
under the preparation registry lock before linking a new ticket, so collection
cannot claim an apparently unpinned PCB between validation and publication.
An abandoned ticket's draft may be reclaimed only after its owner is terminal
and no scheduler reference retains that owner.

TCB allocation, kernel/syscall stacks, user-stack reservation, VFS operations,
message-entry mapping and queue-node allocation belong to the preemptible
preparation phase. The creation spin lock protects only validation and prepared
node publication. Cache IDs before publication: another CPU may run and reclaim
the new thread immediately. Rollback detaches metadata under the lock and frees
resources after unlocking. A caller with nonzero preemption depth is rejected
without consuming its arguments, rather than relaxing its enclosing scope.

ELF loading writes through an explicit target page directory and HHDM, without
borrowing the child's CR3. A timer restores the current TCB's process CR3, so
large copies through child virtual addresses cannot safely remain preemptible.
Only `clone_page_directory(kernel, false)` uses the permanent bootstrap-table
copy path outside the whole-tree lock; other clone modes retain their existing
source-lock protocol. `free_page_directory` requires an unpublished/private or
quiescent, withdrawn address space. It detaches the table under a short lock
and reclaims its private low-half tables afterward, preserving shared high-half
and huge entries. These rules do not solve arbitrary concurrent fork/exec.

`process_execve()` prepares a private ELF image while scheduling is available,
then publishes the complete image in a short `create_thread_lock` section.
Keep filesystem reads, allocation, and other blocking work out of that commit
section.

The process IPC queue object remains stable across exec. Exec detaches its old
message nodes under the queue lock and frees detached nodes after unlocking.
`ipc_send()` transfers message ownership to the queue at publication; sender
code must not read or write the message afterward. Queue enqueue routines copy
the node index before unlocking because a consumer may remove and free the node
immediately.

CLONE_THREAD creation uses the same preparation transaction as native user
threads. The caller's register frame and CLONE_SETTLS base are copied into the
private draft; user TID stores occur outside the publication lock and are
withdrawn on failure. A user-supplied clone stack is never owned by the kernel
thread draft. Preserve the selector-zero rule for an explicit user FS base.

Clearing a child TID is a user TLS/stack reclamation boundary. `kill_thread()`
marks terminal state only. The reaper clears TIDs after scheduler references
show quiescence, including workers in otherwise live processes. Its scan links
a preallocated preparation ticket under create-thread/process-list/preparation
locks, drops those locks, checks CPU references, then performs user-memory I/O.
Collection and exec must keep respecting that ticket. Individual kernel TCBs
retain the existing process-collection lifetime; a cleared TID does not mean a
raw TCB pointer is free to reclaim or retain outside its own lifetime protocol.

Known limitation: simultaneous `execve()` from multiple threads in one process
and stale threads still entering user mode after another thread commits exec
need stronger page-directory lifetime references/deferred reclamation. Do not
claim this race is solved by the boot/readiness or allocator fixes.

## Sheet composition and GUI state

The sheet manager spin lock protects list links, positions, sizes, and types.
Composition does image-sized allocation and copies, so it cannot hold that
lock for the full render. The compositor now:

1. Measures a bounded snapshot under the lock.
2. Allocates snapshot storage with the lock released.
3. Re-locks, verifies the sheet generation, and copies clipped source pixels.
4. Unlocks before alpha composition, peek overlay, framebuffer copy, and free.

Any sheet list, type, position, or size mutation must be performed through the
locked API or a documented locked transaction and advance the generation.
Position/type changes currently use `setBX`, `setBY`, and `change_sheet_type`;
creation initializes an unpublished sheet before linking it, while resize
updates its backing buffer and geometry in a manager-locked transaction.
Unpublished construction is the exception. If a snapshot changes during
capture, reject it and retry; do not dereference sheet/buffer pointers after
unlock unless they are copied or otherwise pinned.

The task-dock spin lock protects metadata only. Rendering icons or refreshing
the compositor while holding it can recursively allocate and acquire unrelated
locks. Copy the needed dock state, unlock, and render from the copy.

## Review and validation checklist

For changes touching these paths:

- Search for every `disable_scheduler()` and prove exact depth restoration on
  all returns and handoffs.
- Search for `malloc`, `free`, VFS, mutex, sleep/yield, decoding, and refresh
  calls inside spin-lock or scheduler-disabled regions.
- Confirm both Ninja generators still link all five allocator wrappers.
- Keep assembly offsets and their static assertions in sync.
- Build with `python3 tools/gen_ninja.py --out build.ninja` and
  `ninja -f build.ninja all`.
- Run relevant unit tests and inspect `git diff --check`.
- For scheduler/GUI startup changes, boot through the GUI login in QEMU,
  inspect serial output, exercise repeated application launches and the
  control center, then verify QEMU exits normally.

Build success alone does not validate scheduler liveness or graphical
interaction.


## Current runtime regression evidence

An earlier QEMU run completed GUI login and desktop startup, launched 15
fmanager instances with 15/15 user-stack builds, then started the control center.
GDB observed the scheduler boot gate open and all four CPU disable depths at zero;
QEMU exited normally. Artifacts were kept outside the source tree under
`/tmp/xj380-fixed-20261002-03/`. The project does not yet have an automated QEMU
regression target, so repeat these steps after changes to the invariants above.

The native ext4 integration regression also passed with the final scheduler,
FD and process-lifetime changes: real first-account creation, a second boot and
password login, desktop/dock startup, 15/15 ordinary file-manager launches and
control-center System/Appearance/App Showcase navigation. Both QEMU sessions
exited normally through QMP with code zero. The four-CPU TCG run reported no
page fault, panic, assertion or recovery tripwire. Evidence is in
`/tmp/xj380-ext4-gui-regression-1k4_872v/result.json`, with serial logs, screenshots
and staged artifact hashes beside it. This is GUI liveness evidence, not a
claim that arbitrary interrupted kernel cleanup is resumable.
