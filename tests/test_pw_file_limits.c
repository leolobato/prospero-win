/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The title's descriptor limit is raised to its hard limit and both values
 * are recorded, as the kernel reports them. */
#include "../native/pw_file_limits.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

static struct rlimit current;
static int get_calls, set_calls, get_fail, set_fail, get_fail_after;
static rlim_t clamp;  /* what the kernel keeps of a raise, 0 for all of it */

static int fake_get(int resource, struct rlimit *limit)
{
    assert(resource == RLIMIT_NOFILE);
    get_calls++;
    if (get_fail || (get_fail_after && get_calls > get_fail_after)) { errno = EPERM; return -1; }
    *limit = current;
    return 0;
}

static int fake_set(int resource, const struct rlimit *limit)
{
    assert(resource == RLIMIT_NOFILE);
    set_calls++;
    if (set_fail) { errno = EPERM; return -1; }
    current = *limit;
    if (clamp && current.rlim_cur > clamp) current.rlim_cur = clamp;
    if (clamp && current.rlim_max > clamp) current.rlim_max = clamp;
    return 0;
}

static const PwFileLimitsOps ops = { fake_get, fake_set };

static void reset(rlim_t soft, rlim_t hard)
{
    current.rlim_cur = soft; current.rlim_max = hard;
    get_calls = set_calls = get_fail = set_fail = get_fail_after = 0;
    clamp = 0;
}

int main(void)
{
    PwFileLimits limits;

    /* soft below hard: raised */
    reset(1024, 4096);
    assert(pw_file_limits_raise_with(&ops, &limits) == 0);
    assert(limits.soft_before == 1024 && limits.hard_before == 4096);
    assert(limits.soft_after == 4096 && limits.hard_after == 4096);
    assert(set_calls == 1 && limits.set_errno == 0 && limits.get_errno == 0);

    /* already equal: nothing to set, the values are still recorded */
    reset(13952, 13952);
    assert(pw_file_limits_raise_with(&ops, &limits) == 0);
    assert(set_calls == 0 && limits.soft_after == 13952 && limits.hard_after == 13952);

    /* the kernel clamps the raise: the values after are the ones it kept */
    reset(256, 13952);
    clamp = 12555;
    assert(pw_file_limits_raise_with(&ops, &limits) == 0);
    assert(limits.soft_before == 256 && limits.hard_before == 13952);
    assert(limits.soft_after == 12555 && limits.hard_after == 12555);

    /* a refused raise is reported and the limits stay as they were */
    reset(256, 1024);
    set_fail = 1;
    assert(pw_file_limits_raise_with(&ops, &limits) == -1);
    assert(limits.set_errno == EPERM && limits.soft_after == 256 && limits.hard_after == 1024);

    /* an unreadable limit leaves everything unknown */
    reset(256, 1024);
    get_fail = 1;
    assert(pw_file_limits_raise_with(&ops, &limits) == -1);
    assert(limits.get_errno == EPERM && limits.soft_before == -1 && limits.soft_after == -1);
    assert(set_calls == 0);

    /* the raise went through but the limit cannot be read again: reported */
    reset(256, 1024);
    get_fail_after = 1;
    assert(pw_file_limits_raise_with(&ops, &limits) == -1);
    assert(set_calls == 1 && limits.get_errno == EPERM);
    assert(limits.soft_before == 256 && limits.soft_after == -1 && limits.hard_after == -1);

    /* unlimited reads as -1 */
    reset(RLIM_INFINITY, RLIM_INFINITY);
    assert(pw_file_limits_raise_with(&ops, &limits) == 0);
    assert(limits.soft_before == -1 && limits.hard_after == -1);

    printf("file limits passed\n");
    return 0;
}
