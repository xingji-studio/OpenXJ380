// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <user/user.h>
#include <xauth.h>

struct process_control_block;
int user_auth_oobe_state();
int user_auth_list(UserInfo *out, int count);
int user_auth_login(const char *username, const char *password, UserInfo *out);
int user_auth_create_first(const char *username, const char *password, UserInfo *out);
int user_auth_begin(const XAuthBegin *request, XAuthChallenge *challenge);
int user_auth_finish(const XAuthRequest *request, XAuthResult *result, unsigned *operation);
int user_auth_account_info(XAccountInfo *info);
void user_auth_process_exit(process_control_block *process);
