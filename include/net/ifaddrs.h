#pragma once

#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_NETWORKING)

struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    union {
        struct sockaddr *ifu_broadaddr;
        struct sockaddr *ifu_dstaddr;
    } ifa_ifu;
    void *ifa_data;
};

#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

#if defined(CONFIG_NET_SOCKETS)

void freeifaddrs(struct ifaddrs *ifp);
int getifaddrs(struct ifaddrs **ifap);

#endif /* CONFIG_NET_SOCKETS */

#endif /* CONFIG_NETWORKING */

#ifdef __cplusplus
}
#endif
