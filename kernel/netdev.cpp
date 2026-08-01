#include <netdev.h>
#include <proto.hpp>
#include <dlinker.h>
#include <cpu/lock.h>

netdev_t *netdevs[MAX_NETDEV_NUM] = {NULL};
static spin_t g_netdev_lock = SPIN_INIT;

void regist_netdev(void *desc, uint8_t *mac, uint32_t mtu, netdev_send_t send,
                   netdev_recv_t recv) {
    if (desc == NULL || mac == NULL || send == NULL || recv == NULL || mtu == 0) {
        return;
    }

    spin_lock(&g_netdev_lock);
    for (int i = 0; i < MAX_NETDEV_NUM; i++) {
        if (netdevs[i] == NULL) {
            netdevs[i] = (netdev_t*)malloc(sizeof(netdev_t));
            if (netdevs[i] == NULL) {
                break;
            }
            netdevs[i]->desc = desc;
            netdevs[i]->mtu = mtu;
            memcpy(netdevs[i]->mac, mac, sizeof(netdevs[i]->mac));
            netdevs[i]->send = send;
            netdevs[i]->recv = recv;
            break;
        }
    }
    spin_unlock(&g_netdev_lock);
}

EXPORT_SYMBOL(regist_netdev);

netdev_t *get_default_netdev() { return netdevs[0]; }
EXPORT_SYMBOL(get_default_netdev);

int netdev_send(netdev_t *dev, void *data, uint32_t len) {
    if (dev == NULL || data == NULL || dev->send == NULL) {
        return -EINVAL;
    }

    if (len == 0) {
        return 0;
    }
    if (len > dev->mtu) {
        return -EMSGSIZE;
    }

    return dev->send(dev->desc, data, len);
}
EXPORT_SYMBOL(netdev_send);

int netdev_recv(netdev_t *dev, void *data, uint32_t len) {
    if (dev == NULL || data == NULL || dev->recv == NULL) {
        return -EINVAL;
    }

    if (len == 0) {
        return 0;
    }

    int ret = dev->recv(dev->desc, data, len);

    return ret;
}
EXPORT_SYMBOL(netdev_recv);
