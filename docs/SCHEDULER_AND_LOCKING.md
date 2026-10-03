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

Never use `enable_scheduler()` to start the system scheduler. Never replace a
save/restore pair with a naked enable: the caller may have entered with a
nonzero depth. A function that changes the depth must restore its entry value
on every error path and before a non-returning user-mode handoff.

`PROCESSOR_INFO.current_task`, `syscall_user_rsp`, and `syscall_user_rax` have
fixed offsets consumed by `kernel/intr/handler.S`. The compile-time assertions
in `kernel/task/scheduler.cpp` are intentional ABI checks. New per-CPU state
belongs after those assembly fields unless every assembly offset is reviewed
and updated together.

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

## Exec and IPC publication

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

The latest recorded QEMU run completed GUI login and desktop startup, launched 15
fmanager instances with 15/15 user-stack builds, then started the control center.
GDB observed the scheduler boot gate open and all four CPU disable depths at zero;
QEMU exited normally. Artifacts were kept outside the source tree under
`/tmp/xj380-fixed-20261002-03/`. The project does not yet have an automated QEMU
regression target, so repeat these steps after changes to the invariants above.
