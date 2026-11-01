// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <task/pcb.h>
#include <xnet.h>

// Immutable registration, independent of the existing socket_provider_t ABI.
// Only the native kernel worker calls these methods. No method may wait for
// network readiness/data; -EAGAIN/-EINPROGRESS defer to a later worker pass.
// close aborts pending TCP output. query does not consume connection errors.
struct XNetProvider
{
    uint32_t size, version;
    uint64_t features;
    int (*open)(const XNetOpenDesc *, void **);
    void (*close)(void *);
    int (*bind)(void *, const XNetEndpoint *);
    int (*listen)(void *, uint32_t);
    int (*accept)(void *, void **, XNetEndpoint *);
    int (*connect)(void *, const XNetEndpoint *);
    int (*shutdown)(void *, uint32_t);
    int (*query)(void *, XNetSocketInfo *);
    int64_t (*send)(void *, const void *, uint32_t, const XNetEndpoint *);
    int64_t (*receive)(void *, void *, uint32_t, XNetDatagramInfo *);
    int (*tcp_options)(void *, XNetTcpOptions *, bool);
    int (*udp_options)(void *, XNetUdpOptions *, bool);
    int (*resolve_start)(const char *, uint32_t, void **);
    int (*resolve_poll)(void *, XNetAddress *);
    void (*resolve_release)(void *);
    int (*interface_info)(XNetInterfaceInfo *, XNetInterfaceAddress *, uint32_t *, XNetDnsConfig *);
    int (*apply_interface)(const XNetInterfaceConfig *);
    int (*apply_dns)(const XNetDnsConfig *);
};

#ifdef __cplusplus
extern "C" {
#endif
int xnet_register_provider(const XNetProvider *provider);
#ifdef __cplusplus
}
#endif
int     xnet_init();
int64_t xnet_syscall(uint64_t request);
void    xnet_process_release(pcb_t process);
void    xnet_thread_release(tcb_t task);
