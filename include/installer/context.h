#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <installer/package.h>
#include <stdint.h>

namespace installer
{
// Kernel-only borrowed view. The verified root package, record arena and
// retained source outlive every use until reboot. This never returns a user
// address and does not publish a root or grant target-write authority.
int session_root_package_view(xjpak2::Plan *plan, uint64_t *epoch);
// Trusted kernel bootstrap only, after root publication and graphics setup.
// Revocation is permanent until reboot. Per-request Admin/owner checks remain.
int session_activate_native();
void session_revoke_native();
} // namespace installer
