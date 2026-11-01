// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <xnet_manager.h>

// VFS functions run only on the permanent XNet worker, without registry locks.
// The caller's CS check happens at syscall submission, before work publication.
int xnet_settings_validate(const XNetSavedConfig &config);
int xnet_settings_read(XNetSavedConfig *config);
int xnet_settings_save(const XNetSavedConfig &config);
