#pragma once

#include <net/socket.h>

// Read-only Linux NETLINK_ROUTE link/address dumps over socketfs. The network
// module's existing netinfo snapshot supplies the configured Ethernet data.
extern const socket_provider_t route_netlink_provider;
int                            route_netlink_open(int type, int protocol);
