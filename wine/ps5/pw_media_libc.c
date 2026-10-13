/* SPDX-License-Identifier: LGPL-2.1-or-later
 * GIO requires resolver-record symbols even for a local-file-only media build.
 * The payload SDK declares these BSD entry points but does not implement them.
 * DNS records are unsupported here: return a permanent error without touching
 * the answer. This does not replace the SDK's getaddrinfo/getnameinfo support.
 */
#include <errno.h>
#include <netdb.h>

int __res_query(const char *name, int qclass, int type,
                unsigned char *answer, int length)
{
    (void)name; (void)qclass; (void)type; (void)answer; (void)length;
    h_errno = NO_RECOVERY;
    errno = ENOSYS;
    return -1;
}

int __dn_expand(const unsigned char *message, const unsigned char *end,
                const unsigned char *compressed, char *expanded, int length)
{
    (void)message; (void)end; (void)compressed; (void)expanded; (void)length;
    errno = ENOSYS;
    return -1;
}

/* The SDK also lacks the fstab enumeration API. Local media does not need
 * administrative mount configuration; report that operation as unsupported. */
struct fstab;
int setfsent(void)
{
    errno = ENOSYS;
    return 0;
}

struct fstab *getfsent(void)
{
    errno = ENOSYS;
    return 0;
}

void endfsent(void)
{
}
