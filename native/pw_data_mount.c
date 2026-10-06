/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pw_data_mount.h"
#include "pw_lapy_elevation.h"

#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* XSI; hidden by _POSIX_C_SOURCE in the PS5 SDK headers. */
int setreuid(uid_t ruid, uid_t euid);

int pw_data_mount_request_with(const PwDataMountOps *ops, int32_t pid,
                               int max_wait_ms, PwDataMountResult *detail)
{
    PwDataMountResult local;
    uid_t original_effective_uid;

    if (!detail) detail = &local;
    detail->data_before = detail->helper_completed = detail->helper_errno = 0;
    detail->credential_errno = 0;
    detail->data_after = detail->waited_ms = detail->settled_ms = 0;
    if (!ops || !ops->request || !ops->data_visible || !ops->sleep_ms ||
        pid <= 1 || max_wait_ms < 0) return -1;

    /* A new LoadExec process starts fresh, but an existing grant is reusable. */
    if (ops->data_visible()) {
        detail->data_before = detail->data_after = 1;
        return 0;
    }

    original_effective_uid = geteuid();
    errno = 0;
    if (ops->request(pid) == 0)
        detail->helper_completed = 1;
    else
        detail->helper_errno = errno ? errno : EIO;

    /* The helper grants the root view but changes EUID. Keep private bottle
     * ownership usable before any managed path access, even on request failure. */
    if (geteuid() != original_effective_uid && seteuid(original_effective_uid) != 0) {
        detail->credential_errno = errno ? errno : EACCES;
        errno = detail->credential_errno;
        return -1;
    }
    if (geteuid() != original_effective_uid) {
        detail->credential_errno = EACCES;
        errno = EACCES;
        return -1;
    }
    /* It also leaves the real UID at 0. Wine's server refuses a prefix that
     * is not owned by the real UID, and private bottles belong to the app
     * user, so return the real UID to it too. Nothing later regains root. */
    if (getuid() != original_effective_uid &&
        setreuid(original_effective_uid, (uid_t)-1) != 0) {
        detail->credential_errno = errno ? errno : EACCES;
        errno = detail->credential_errno;
        return -1;
    }
    if (getuid() != original_effective_uid) {
        detail->credential_errno = EACCES;
        errno = EACCES;
        return -1;
    }

    /* A successful wire response is necessary, then confirm actual access. */
    if (!detail->helper_completed) return -1;
    for (;;) {
        if (ops->data_visible()) {
            detail->data_after = 1;
            ops->sleep_ms(PW_DATA_MOUNT_SETTLE_MS);
            detail->settled_ms = PW_DATA_MOUNT_SETTLE_MS;
            return 0;
        }
        if (detail->waited_ms >= max_wait_ms) return -1;
        ops->sleep_ms(PW_DATA_MOUNT_POLL_MS);
        detail->waited_ms += PW_DATA_MOUNT_POLL_MS;
    }
}

static int native_helper_request(int32_t pid)
{
#if defined(PW_DATA_MOUNT_HOST_TEST)
    (void)pid;
    errno = ENOENT;
    return -1;
#else
    return pw_lapy_elevation_request(pid);
#endif
}

static int native_data_visible(void)
{
    struct stat st;
    return stat(PW_DATA_MOUNT_PATH, &st) == 0 && S_ISDIR(st.st_mode);
}

static void native_sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    (void)nanosleep(&ts, NULL);
}

int pw_data_mount_request(PwDataMountResult *detail)
{
    static const PwDataMountOps ops = {
        native_helper_request, native_data_visible, native_sleep_ms};
    return pw_data_mount_request_with(&ops, (int32_t)getpid(),
                                      PW_DATA_MOUNT_WAIT_MS, detail);
}
