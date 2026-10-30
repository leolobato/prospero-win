/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_file_limits.h"

#include <errno.h>

static long long limit_value(rlim_t value)
{
    return value == RLIM_INFINITY ? -1 : (long long)value;
}

int pw_file_limits_raise_with(const PwFileLimitsOps *ops, PwFileLimits *limits)
{
    struct rlimit limit;

    limits->soft_before = limits->hard_before = limits->soft_after = limits->hard_after = -1;
    limits->get_errno = limits->set_errno = 0;
    errno = 0;
    if (ops->get(RLIMIT_NOFILE, &limit)) {
        limits->get_errno = errno ? errno : EIO;
        return -1;
    }
    limits->soft_before = limit_value(limit.rlim_cur);
    limits->hard_before = limit_value(limit.rlim_max);
    if (limit.rlim_cur != limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
        errno = 0;
        if (ops->set(RLIMIT_NOFILE, &limit)) limits->set_errno = errno ? errno : EIO;
    }
    /* The kernel may clamp what it was given (FW 13.60: 13952 reads back
     * as 12555), so the values after are read, not assumed. */
    errno = 0;
    if (ops->get(RLIMIT_NOFILE, &limit)) {
        limits->get_errno = errno ? errno : EIO;
        return -1;
    }
    limits->soft_after = limit_value(limit.rlim_cur);
    limits->hard_after = limit_value(limit.rlim_max);
    return limit.rlim_cur == limit.rlim_max ? 0 : -1;
}

int pw_file_limits_raise(PwFileLimits *limits)
{
    static const PwFileLimitsOps ops = { getrlimit, setrlimit };
    return pw_file_limits_raise_with(&ops, limits);
}
