/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The interface name calls nsiproxy's Unix side (nsiproxy.prx) makes that
 * the payload SDK's libc lacks: if_nameindex and if_freenameindex are in no
 * stub, and ntdll's if_nametoindex (pw_wine_compat) always fails. ndis.c
 * lists the interfaces with them, then asks the kernel about each index
 * (NET_RT_IFLIST sysctl, ioctls), so the indices must be the kernel's.
 *
 * Both come from getifaddrs, which the title's libc exports: one entry per
 * interface name, in the order getifaddrs gives them, with the index of its
 * AF_LINK address (the kernel's), or, where there is none (glibc's AF_PACKET,
 * in the host test), the smallest index no other entry has, so it never
 * shadows a real one (ndis.c keeps one interface per index). If getifaddrs fails the
 * list is NULL with its errno, which ndis.c reports and turns into an empty
 * table, so a caller gets no interfaces at once instead of no answer.
 *
 * The host test builds this file with these names prefixed (Makefile), so
 * the C library's stay in place. */
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#ifdef AF_LINK
#include <net/if_dl.h>
#endif

static unsigned int link_index(const struct ifaddrs *entry)
{
#ifdef AF_LINK
    if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_LINK)
        return ((const struct sockaddr_dl *)entry->ifa_addr)->sdl_index;
#else
    (void)entry;
#endif
    return 0;
}

/* Gives each of the first count entries without an index the smallest one
 * no entry has yet. */
static void fill_missing_indices(struct if_nameindex *list, size_t count)
{
    size_t i, j;
    unsigned int candidate = 1;

    for (i = 0; i < count; i++)
    {
        if (list[i].if_index) continue;
        for (;;)
        {
            for (j = 0; j < count && list[j].if_index != candidate; j++) {}
            if (j == count) break;
            candidate++;
        }
        list[i].if_index = candidate++;
    }
}

struct if_nameindex *if_nameindex(void)
{
    struct ifaddrs *addresses, *entry;
    struct if_nameindex *list;
    size_t count = 0, i;

    if (getifaddrs(&addresses)) return NULL;
    for (entry = addresses; entry; entry = entry->ifa_next) count++;
    if (!(list = calloc(count + 1, sizeof(*list))))
    {
        freeifaddrs(addresses);
        errno = ENOMEM;
        return NULL;
    }
    count = 0;
    for (entry = addresses; entry; entry = entry->ifa_next)
    {
        unsigned int index = link_index(entry);
        if (!entry->ifa_name) continue;
        for (i = 0; i < count && strcmp(list[i].if_name, entry->ifa_name); i++) {}
        if (i == count)
        {
            if (!(list[count].if_name = strdup(entry->ifa_name)))
            {
                freeifaddrs(addresses);
                if_freenameindex(list);
                errno = ENOMEM;
                return NULL;
            }
            count++;
        }
        if (index) list[i].if_index = index;
    }
    freeifaddrs(addresses);
    fill_missing_indices(list, count);
    return list;
}

void if_freenameindex(struct if_nameindex *list)
{
    struct if_nameindex *entry;

    if (!list) return;
    for (entry = list; entry->if_name; entry++) free(entry->if_name);
    free(list);
}

unsigned int if_nametoindex(const char *name)
{
    struct if_nameindex *list = if_nameindex(), *entry;
    unsigned int index = 0;

    if (!list) return 0;
    for (entry = list; entry->if_name; entry++)
        if (!strcmp(entry->if_name, name)) index = entry->if_index;
    if_freenameindex(list);
    if (!index) errno = ENXIO;
    return index;
}
