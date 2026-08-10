#pragma once

/* Host-friendly policy function for node ownership/mode changes.
 *
 * The kernel implementation lives in kernel/syscall/sys.cpp: node_change_permitted().
 * The two must stay in sync: if you change one, mirror the change in the other
 * and update tests/test_security_review_regressions.py accordingly.
 *
 * Intentionally self-contained: this header pulls in neither <stdint.h> nor
 * any other freestanding-incompatible header, so it is safe to compile in
 * both the kernel build (which shadows the system stdint.h) and a hosted
 * unit test that links the host's libc.
 *
 * Parameters:
 *   - current_uid: the effective UID of the calling task.
 *   - is_root:     true if current_uid == 0. (Provided separately so the
 *                  kernel implementation can short-circuit on the root
 *                  identity without re-deriving it.)
 *   - node_owner:  the file's stored owner UID.
 *   - owner_change / group_change: passed unchanged from the chown-style
 *                  callers so the signature matches the kernel helper. They
 *                  currently do not alter the verdict, but the kernel still
 *                  relies on them being passed for future policy tightening.
 */
typedef unsigned int xj_uid_t;

static inline int xj_node_change_permitted(xj_uid_t current_uid,
                                           int is_root,
                                           xj_uid_t node_owner,
                                           int owner_change,
                                           int group_change)
{
    (void)owner_change;
    (void)group_change;
    if (is_root) return 1;
    if (current_uid == node_owner) return 1;
    return 0;
}