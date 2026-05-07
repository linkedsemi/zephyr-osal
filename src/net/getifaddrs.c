#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_NETWORKING) && defined(CONFIG_NET_SOCKETS)

#include "ifaddrs.h"
#include "if.h"

/**
 * @brief Context structure for building ifaddrs linked list
 */
struct ifaddrs_ctx {
    struct ifaddrs *ifaddrs;
    struct ifaddrs *last;
};

/* Forward declarations of interface address handlers */
static void add_link_addr(struct net_if *iface, struct ifaddrs_ctx *ctx,
              const char *ifname, unsigned int flags);

#if defined(CONFIG_NET_IPV4)
static void add_ipv4_addrs(struct net_if *iface, struct ifaddrs_ctx *ctx,
               const char *ifname, unsigned int flags);
#endif

#if defined(CONFIG_NET_IPV6)
static void add_ipv6_addrs(struct net_if *iface, struct ifaddrs_ctx *ctx,
               const char *ifname, unsigned int flags);
#endif

/**
 * @brief Callback invoked for each network interface during iteration
 * @param iface Pointer to Zephyr network interface
 * @param user_data Pointer to ifaddrs context
 */
static void ifaddrs_cb(struct net_if *iface, void *user_data)
{
    struct ifaddrs_ctx *ctx = user_data;
    char ifname[IF_NAMESIZE] = { 0 };
    unsigned int flags = 0;

    /* Get interface name */
    if (net_if_get_name(iface, ifname, sizeof(ifname)) <= 0) {
        strncpy(ifname, "unknown", sizeof(ifname) - 1);
        ifname[sizeof(ifname) - 1] = '\0';
    }

    /* Set interface flags */
    /* IFF_UP: administratively up */
    if (net_if_is_up(iface)) {
        flags |= IFF_UP;
    }
    
    /* IFF_RUNNING: carrier ready */
    if (net_if_flag_is_set(iface, NET_IF_LOWER_UP)) {
        flags |= IFF_RUNNING;
    }
    
    /* IFF_LOOPBACK: loopback interface */
    if (net_if_l2(iface) == &NET_L2_GET_NAME(DUMMY)) {
        flags |= IFF_LOOPBACK;
    }
    
    /* IFF_POINTOPOINT: p2p link */
    if (net_if_flag_is_set(iface, NET_IF_POINTOPOINT)) {
        flags |= IFF_POINTOPOINT;
    }
    
    /* IFF_BROADCAST: supports broadcast (non-P2P) */
    if (!net_if_flag_is_set(iface, NET_IF_POINTOPOINT)) {
        flags |= IFF_BROADCAST;
    }
    
    /* IFF_MULTICAST: supports multicast */
    flags |= IFF_MULTICAST;
    
    /* IFF_NOARP: no ARP for loopback/P2P */
    if ((flags & IFF_LOOPBACK) || (flags & IFF_POINTOPOINT)) {
        flags |= IFF_NOARP;
    }
    
    /* IFF_PROMISC: promiscuous mode */
    if (net_if_flag_is_set(iface, NET_IF_PROMISC)) {
        flags |= IFF_PROMISC;
    }

    /* Populate address entries for this interface */
    add_link_addr(iface, ctx, ifname, flags);

#if defined(CONFIG_NET_IPV4)
    add_ipv4_addrs(iface, ctx, ifname, flags);
#endif

#if defined(CONFIG_NET_IPV6)
    add_ipv6_addrs(iface, ctx, ifname, flags);
#endif
}

/**
 * @brief Get network interface addresses (POSIX compliant)
 * @param ifap Output pointer to store ifaddrs list head
 * @return 0 on success, -1 on error
 */
int getifaddrs(struct ifaddrs **ifap)
{
    struct ifaddrs_ctx ctx = { NULL, NULL };

    if (!ifap)
        return -1;

    /* Iterate all network interfaces and build ifaddrs list */
    net_if_foreach(ifaddrs_cb, &ctx);
    *ifap = ctx.ifaddrs;

    return ctx.ifaddrs ? 0 : -1;
}

/**
 * @brief Free the ifaddrs linked list
 * @param ifa Head of the ifaddrs list
 */
void freeifaddrs(struct ifaddrs *ifa)
{
    struct ifaddrs *next;

    while (ifa) {
        next = ifa->ifa_next;
        free(ifa);
        ifa = next;
    }
}

/**
 * @brief Allocate and initialize one ifaddrs entry
 * @param ctx ifaddrs list context
 * @param ifname Interface name
 * @param flags Interface flags
 * @param af Address family (AF_INET, AF_INET6, AF_PACKET)
 * @param saddr Source address
 * @param smask Source netmask
 * @param bcast Source broadcast address
 * @return true on success, false on failure
 */
static bool add_ifaddr(struct ifaddrs_ctx *ctx, const char *ifname,
               unsigned int flags, int af,
               const void *saddr, const void *smask, const void *bcast)
{
    size_t len_name = strlen(ifname) + 1;
    size_t len_addr = sizeof(struct sockaddr_in);
    size_t len_6addr = sizeof(struct sockaddr_in6);
    size_t total;

    /* Use IPv6 structure size for AF_INET6 */
    len_addr = (af == AF_INET6) ? len_6addr : len_addr;

    /* Allocate single block for ifaddrs + name + addr + mask + broadcast */
    total = sizeof(struct ifaddrs)
        + len_name
        + len_addr
        + len_addr
        + len_addr;

    struct ifaddrs *ifa = calloc(1, total);
    if (!ifa)
        return false;

    uint8_t *ptr = (uint8_t *)ifa;

    /* Assign interface name */
    ifa->ifa_name = (char *)(ptr + sizeof(struct ifaddrs));
    strcpy(ifa->ifa_name, ifname);

    /* Assign address pointers within the allocated block */
    ifa->ifa_addr      = (struct sockaddr *)(ptr + sizeof(struct ifaddrs) + len_name);
    ifa->ifa_netmask   = (struct sockaddr *)(ptr + sizeof(struct ifaddrs) + len_name + len_addr);
    ifa->ifa_broadaddr = (struct sockaddr *)(ptr + sizeof(struct ifaddrs) + len_name + len_addr + len_addr);

    /* Set flags */
    ifa->ifa_flags = flags;

    /* Copy address data if provided */
    if (saddr)
        memcpy(ifa->ifa_addr, saddr, len_addr);
    if (smask)
        memcpy(ifa->ifa_netmask, smask, len_addr);
    if (bcast && (flags & IFF_BROADCAST))
        memcpy(ifa->ifa_broadaddr, bcast, len_addr);

    /* Chain to linked list */
    if (!ctx->ifaddrs)
        ctx->ifaddrs = ifa;
    else
        ctx->last->ifa_next = ifa;

    ctx->last = ifa;

    return true;
}

/**
 * @brief Add link-layer (MAC) address entry
 * @param iface Zephyr network interface
 * @param ctx ifaddrs list context
 * @param ifname Interface name
 * @param flags Interface flags
 */
static void add_link_addr(struct net_if *iface, struct ifaddrs_ctx *ctx,
              const char *ifname, unsigned int flags)
{
    struct net_linkaddr *ll = net_if_get_link_addr(iface);
    struct sockaddr sa = {0};

    if (!ll)
        return;

    sa.sa_family = AF_PACKET;
    memcpy(sa.data, ll->addr, ll->len);

    add_ifaddr(ctx, ifname, flags, AF_PACKET, &sa, NULL, NULL);
}

#if defined(CONFIG_NET_IPV4)
/**
 * @brief Add IPv4 unicast addresses
 * @param iface Zephyr network interface
 * @param ctx ifaddrs list context
 * @param ifname Interface name
 * @param flags Interface flags
 */
static void add_ipv4_addrs(struct net_if *iface, struct ifaddrs_ctx *ctx,
               const char *ifname, unsigned int flags)
{
    struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
    struct sockaddr_in bcast = { 0 };

    if (!ipv4)
        return;

    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        struct net_if_addr_ipv4 *ifa = &ipv4->unicast[i];

        if (!ifa->ipv4.is_used)
            continue;

        struct sockaddr_in addr = { 0 };
        struct sockaddr_in mask = { 0 };

        addr.sin_family = AF_INET;
        addr.sin_addr = ifa->ipv4.address.in_addr;

        mask.sin_family = AF_INET;
        mask.sin_addr = ifa->netmask;

        /* Calculate IPv4 broadcast address */
        bcast.sin_family = AF_INET;
        bcast.sin_addr.s_addr = addr.sin_addr.s_addr | ~mask.sin_addr.s_addr;

        /* Add to ifaddrs list with broadcast flag */
        add_ifaddr(ctx, ifname, flags | IFF_BROADCAST,
               AF_INET, &addr, &mask, &bcast);
    }
}
#endif /* CONFIG_NET_IPV4 */

#if defined(CONFIG_NET_IPV6)
/**
 * @brief Add IPv6 unicast addresses
 * @param iface Zephyr network interface
 * @param ctx ifaddrs list context
 * @param ifname Interface name
 * @param flags Interface flags
 */
static void add_ipv6_addrs(struct net_if *iface, struct ifaddrs_ctx *ctx,
               const char *ifname, unsigned int flags)
{
    struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;

    if (!ipv6)
        return;

    for (int i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
        struct net_if_addr *ifa = &ipv6->unicast[i];

        if (!ifa->is_used)
            continue;

        struct sockaddr_in6 addr = {0};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = ifa->address.in6_addr;

        add_ifaddr(ctx, ifname, flags, AF_INET6, &addr, NULL, NULL);
    }
}
#endif /* CONFIG_NET_IPV6*/

#endif /* CONFIG_NET_SOCKETS && CONFIG_NETWORKING */
