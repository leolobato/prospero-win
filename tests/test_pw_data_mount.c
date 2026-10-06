/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host-side checks for helper completion, /data visibility and settle timing. */
#include "../native/pw_data_mount.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>

static int request_calls, request_fail, visible_in, poll_calls, sleep_total;
static int32_t requested_pid;
static uid_t effective_uid = 1, real_uid = 1;
static int restore_fail, restore_no_effect, real_restore_fail;
/* Model the helper's real UID transition, without changing host privileges. */
uid_t geteuid(void) { return effective_uid; }
int seteuid(uid_t uid) {
    if (restore_fail) { errno = EPERM; return -1; }
    if (!restore_no_effect) effective_uid = uid;
    return 0;
}
/* On the console the helper also leaves the real UID at 0; Wine's server
 * requires the prefix owner to equal the real UID. */
uid_t getuid(void) { return real_uid; }
int setreuid(uid_t ruid, uid_t euid) {
    if (real_restore_fail) { errno = EPERM; return -1; }
    /* FreeBSD: without root, a new real UID must equal the current real UID
     * (the saved UID is 0 here, as the helper left it). */
    if (effective_uid != 0 && ruid != (uid_t)-1 && ruid != real_uid && ruid != 0) { errno = EPERM; return -1; }
    if (ruid != (uid_t)-1) real_uid = ruid;
    if (euid != (uid_t)-1) effective_uid = euid;
    return 0;
}

static int fake_request(int32_t pid)
{
    effective_uid = 0; real_uid = 0;
    request_calls++;
    requested_pid = pid;
    if (request_fail) { errno = ETIMEDOUT; return -1; }
    return 0;
}

static int fake_visible(void)
{
    poll_calls++;
    if (visible_in > 0) { visible_in--; return 0; }
    return 1;
}

static void fake_sleep(int ms) { sleep_total += ms; }
static int hidden_once;
static int fake_hidden_then_visible(void) { return hidden_once++ ? 1 : 0; }

int main(void)
{
    static const PwDataMountOps ops = {fake_request, fake_visible, fake_sleep};
    PwDataMountResult r;

    request_calls = poll_calls = sleep_total = 0;
    request_fail = 0;
    visible_in = 3;
    assert(pw_data_mount_request_with(&ops, 2595, PW_DATA_MOUNT_WAIT_MS, &r) == 0);
    assert(effective_uid == 1 && real_uid == 1);
    assert(request_calls == 1 && requested_pid == 2595);
    assert(r.data_before == 0 && r.helper_completed == 1 && r.helper_errno == 0);
    assert(r.data_after == 1 && r.waited_ms == 2 * PW_DATA_MOUNT_POLL_MS);
    assert(r.settled_ms == PW_DATA_MOUNT_SETTLE_MS);
    assert(sleep_total == r.waited_ms + r.settled_ms);

    /* Already reachable: no helper request or delay. */
    request_calls = poll_calls = sleep_total = 0;
    visible_in = 0;
    assert(pw_data_mount_request_with(&ops, 2595, PW_DATA_MOUNT_WAIT_MS, &r) == 0);
    assert(r.data_before == 1 && r.data_after == 1 && request_calls == 0);
    assert(sleep_total == 0);

    /* A completed helper that fails to expose /data times out. */
    request_calls = poll_calls = sleep_total = 0;
    visible_in = 1000000;
    assert(pw_data_mount_request_with(&ops, 7, 300, &r) == -1);
    assert(request_calls == 1 && r.helper_completed == 1);
    assert(r.data_after == 0 && r.waited_ms == 300 && r.settled_ms == 0);

    /* No completion response means no access and no settle delay. */
    request_calls = poll_calls = sleep_total = 0;
    request_fail = 1;
    visible_in = 1000000;
    assert(pw_data_mount_request_with(&ops, 7, 300, &r) == -1);
    assert(effective_uid == 1);
    assert(r.helper_completed == 0 && r.helper_errno == ETIMEDOUT);
    assert(r.data_after == 0 && sleep_total == 0);
    request_fail = 0;

    /* The real UID must also return, or private bottles are "not owned". */
    request_calls = poll_calls = sleep_total = 0;
    visible_in = 0; real_restore_fail = 1;
    effective_uid = real_uid = 1;
    {
        static const PwDataMountOps hidden = {fake_request, fake_hidden_then_visible, fake_sleep};
        assert(pw_data_mount_request_with(&hidden, 7, 300, &r) == -1);
    }
    assert(r.credential_errno == EPERM && real_uid == 0);
    real_restore_fail = 0; effective_uid = real_uid = 1;

    /* Even wire success must fail closed if the original UID cannot return. */
    restore_fail = 1;
    effective_uid = 1;
    visible_in = 1;
    assert(pw_data_mount_request_with(&ops, 7, 300, &r) == -1);
    assert(errno == EPERM && r.credential_errno == EPERM && r.data_after == 0);
    restore_fail = 0;
    restore_no_effect = 1;
    effective_uid = 1;
    visible_in = 1;
    assert(pw_data_mount_request_with(&ops, 7, 300, &r) == -1);
    assert(errno == EACCES && r.credential_errno == EACCES && r.data_after == 0);
    restore_no_effect = 0;
    effective_uid = 1;

    assert(pw_data_mount_request_with(NULL, 7, 100, &r) == -1);
    assert(pw_data_mount_request_with(&ops, 1, 100, &r) == -1);
    puts("data mount: helper result, access visibility, deadline and settle delay covered");
    return 0;
}
