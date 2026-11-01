// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "xnet.h"

#define XNET_OP_CAPS           0U
#define XNET_OP_OPEN           1U
#define XNET_OP_BIND           2U
#define XNET_OP_LISTEN         3U
#define XNET_OP_ACCEPT         4U
#define XNET_OP_CONNECT        5U
#define XNET_OP_SEND           6U
#define XNET_OP_RECEIVE        7U
#define XNET_OP_SEND_TO        8U
#define XNET_OP_RECEIVE_FROM   9U
#define XNET_OP_SHUTDOWN       10U
#define XNET_OP_QUERY          11U
#define XNET_OP_CLOSE          12U
#define XNET_OP_WAIT           13U
#define XNET_OP_CANCEL_CREATE  14U
#define XNET_OP_CANCEL_SIGNAL  15U
#define XNET_OP_RESOLVE_START  16U
#define XNET_OP_RESOLVE_RESULT 17U
#define XNET_OP_ENUM           18U
#define XNET_OP_INTERFACE      19U
#define XNET_OP_ADDRESSES      20U
#define XNET_OP_DNS_GET        21U
#define XNET_OP_CONFIG_APPLY   22U
#define XNET_OP_DNS_APPLY      23U
#define XNET_OP_WATCH_CREATE   24U
#define XNET_OP_WATCH_READ     25U
#define XNET_OP_TCP_SET        26U
#define XNET_OP_TCP_GET        27U
#define XNET_OP_UDP_SET        28U
#define XNET_OP_UDP_GET        29U
#define XNET_OP_CONFIG_READ    30U
#define XNET_OP_CONFIG_SAVE    31U
#define XNET_OP_CONFIG_RELOAD  32U
#define XNET_OP_CONFIG_STATUS  33U

// The syscall copies this envelope and each pointed input before publication.
// input/output/auxiliary are user virtual addresses, never backend pointers.
typedef struct XNetCall
{
    uint32_t      size, version, operation, flags;
    XNetHandle    object;
    uint64_t      input, output, auxiliary, count, argument;
    XNetIoOptions io;
} XNetCall;
