# Linux inotify compatibility

`kernel/task/inotify.cpp` is a first-party Apache-2.0 backend for Linux
`inotify_init`, `inotify_init1`, `inotify_add_watch`, and `inotify_rm_watch`.
The documented ABI constants and 16-byte event header follow the Linux
[inotify API](https://man7.org/linux/man-pages/man7/inotify.7.html). No Linux
kernel implementation or new linked dependency is imported.

Anonymous VFS stream nodes use the existing FD operation/retirement contract.
Dup/fork share the vnode instance; the last retired descriptor closes it after
outstanding operations drain. There is no public vnode, callback, FD-handle,
process, or module ABI layout change. `O_NONBLOCK`, `O_CLOEXEC`, poll readiness,
`FIONREAD`, whole-record reads, and failed-user-copy retention are implemented.
A racing reader commits only the head generation it copied to user memory.

A watch retains its vnode identity, using mount/device/inode and a vnode fallback
for inode-less pseudo-filesystems. Duplicate inode watches in one instance
reuse a descriptor, with MASK_ADD/MASK_CREATE, ONLYDIR, DONT_FOLLOW,
EXCL_UNLINK, and ONESHOT handling. Watch descriptors increase without reuse
within an instance. The instance allows 4096 live watches and 256 ordinary
queued events plus a reserved IN_Q_OVERFLOW record. Identical adjacent events
coalesce; producers never overwrite queued records.

The namespace lock precedes the private notification spin lock. Notification
producers only copy bounded identities, names and queue records. They never
allocate, enter VFS, touch user memory, wait, or free storage while locked.
Removed/one-shot watches are detached under the registry lock and their vnode
pins are disposed in ordinary task context by add/remove/read/final close.
The ext4 rename glue publishes moves at the existing namespace commit, before
old names are freed. Paired directory moves share a nonzero cookie; overwritten
inode watches receive DELETE_SELF/IGNORED. Managed RAM-root renames and VFS
virtual-symlink renames use the same publication contract.

The currently delivered events are CREATE, MODIFY (write/resize), DELETE,
MOVED_FROM, MOVED_TO, MOVE_SELF, DELETE_SELF, IGNORED, ISDIR, and Q_OVERFLOW.
This is sufficient for the accepted Node.js native fs.watch/chokidar workflows.
It is not the complete Linux event model: metadata-only ATTRIB, OPEN, ACCESS,
CLOSE, successful-unmount notification, last-hardlink lifetime semantics, and
legacy FAT/pseudo-filesystem rename integration remain unverified or pending.
Unimplemented event bits are accepted as part of Linux masks, but their delivery
is not claimed. Watch vnode pins currently participate in native unmount busy
checks; callers must close them before unmounting. No polling fallback is used
to hide those boundaries.

`tests/test_inotify.py` in the product repository executes this actual backend
with ASan/UBSan and checked host adapters. It covers masks, names/record padding,
EAGAIN, small buffers, EFAULT retention, readiness/FIONREAD, paired rename cookies,
one-shot/removal, duplicate inode watches, coalescing, overflow, competing
readers, and final close racing notification producers. Product QEMU acceptance
uses unchanged stock Node.js with native fs.watch and unchanged nodemon/chokidar.
