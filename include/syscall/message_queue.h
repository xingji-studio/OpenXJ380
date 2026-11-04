// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <syscall/pxapi.h>

// Caller pins the window/task while acquiring its current paired pipe endpoints.
// Successful posting transfers a copied record and a descriptor ticket, never a
// task/process pointer. Returns zero or -errno; a full queue preserves old FIFO.
bool message_window_channel_valid(tcb_t task, uint64_t *inode_out = NULL);
int message_post_window(tcb_t task, const MessageInfoFormat &message, uint64_t expected_inode = 0);
int message_post_notification(uint64_t pid, uint64_t notification_id, uint64_t action_id);
