# procfs process and cache lifetime

The original Apache-2.0 procfs observation protocol separates a persistent
process identity from the PCB that supplies live values. It adds no external
implementation or dependency and does not change the VFS callback layout,
PCB/TCB layout or scheduler assembly offsets.

`proc_handle_t::process` retains a heap context. Its compatibility `task` prefix
is NULL in persistent handles. A heap operation ticket captures a PCB only while
that context is live; every callback releases the ticket after its last process
access. New contexts are registered while the process-list lock proves that
the PCB is still published and is not in exclusive collection. All allocation,
VFS work, formatting and waiting occurs after the short registry locks release.

Collection withdraws the PCB from the process list before
`procfs_on_exit_task`. The hook closes the context's admission gate, removes the
PID directory name, then waits for existing live operations in ordinary task
context. It never waits while holding a spin lock. A descriptor that retains an
old proc node keeps the detached subtree and context alive, and subsequent
read/stat/readlink operations return ENOENT. They do not dereference the old PCB
or fall back to the current process. Such descriptors do not keep the PCB alive
after the current operations complete.

Operation tickets carry their owning TCB. Dead-owner reclamation requires both
DEATH/OUT and absence from scheduler current, selected and previous-stack
references. FD registry cleanup precedes dropping procfs operation references.
`kill_thread0` also removes the terminal owner's procfs tickets after its FD
tickets, before the TCB can be reused. No owner pointer escapes the registry
lock for a later quiescence query. A failed fork withdraws and retires any lazy
proc context before destroying its partially created process storage.

The reaper retries retired subtrees with `vfs_try_free_detached`, outside all
registry/backend locks. A retained mounted-root reference covers the gap from
name removal through final disposal. Busy trees rotate through the queue so a
held descriptor cannot starve every later retirement. Checked disposal closes
all backend handles, including directory handles, before releasing storage.
Private construction failures use that same checked disposal; an ordinary
directory-reference close does not promise to destroy its backend handle.

`/proc/PID/fd` snapshots numeric descriptor names under the existing FD registry
and table locks, allocates/prepares nodes after unlocking, and publishes fully
initialized nodes. It prunes obsolete names through checked VFS retirement.
An opened proc link remains valid when a refresh cannot prune it. A readlink
acquires an operation ticket for the target process's current descriptor, so
close/dup replacement cannot release that handle mid-read. The FD ticket is
released before the process observation ticket. Mapping reports also use FD
tickets instead of retaining a bare descriptor-table pointer.

The thread-creation notification does no allocation or VFS work inside creator
spin locks or scheduler-disabled sections. Ordinary root enumeration and
`/proc/self` resolution lazily construct a complete private PID subtree, then
publish it once. Competing creators release their unpublished tree. A context
retired during construction rejects publication. Self links resolve the current
process on each lookup rather than cache a previous process's directory.

The current procfs contents are read-only observation interfaces. The VFS
read-only property rejects create, delete, rename and data write before any
cache mutation; internal dynamic node publication remains available. The old
successful dummy mutation callbacks did not implement a writable procfs ABI.

Mounting is a bootstrap singleton. `procfs_setup` owns a private current-TCB/TID
scope while the boot scheduler gate remains closed. `procfs_mount` requires
that exact scope as well as the kernel registration token; a token translated
by the user mount syscall does not grant that scope. Runtime first mounts and
duplicate mounts are refused. The entire global tree is prepared privately.
The old empty mountpoint backend is checked-closed with its original fsid before
one publication installs the new root and callback handles. Failure preserves
the original fsid and does not publish a proc root. This does not implement
runtime multi-mount reservation or a general mount transaction API.

## Evidence and limits

`python3 -m unittest discover -s tests -p test_procfs_lifetime.py -v` compiles
actual procfs context/callback/cache bodies together with actual VFS retirement
and FD registry code under ASan and UBSan. Only platform IRQ/scheduler behavior
and unrelated process-information formatters are seams. Tests cover PCB deletion
with retained nodes, live-operation waiting, selected no-unwind callback cuts,
scheduler-reference refusal, FD-first cleanup, concurrent descriptor replacement,
refresh, dynamic self resolution, readonly mutation refusal, duplicate/close
reference balancing, private-publication/exit races, allocation failures,
bootstrap authorization and foreign mountpoint backend-close errors.

These tests establish collection and descriptor/cache lifetime. They do not
establish a consistent snapshot during concurrent live exec or VMA mutation.
Those mutable subobjects require their own synchronization protocol. An exit
waiter or cache collector terminated halfway through collection may retain its
exclusive unfinished state until reboot; another task must not resume arbitrary
interrupted frees. The selected callback cuts do not prove reclamation of every
temporary allocation at every possible kernel instruction. No exhaustive
scheduler, whole-kernel leak or device-DMA cancellation claim is made.
