/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_title_sandbox.h"

/* Appends text at offset used; the new offset, or size when it does not fit. */
static size_t append(char *out, size_t size, size_t used, const char *text)
{
    while (*text) {
        if (used + 1 >= size) return size;
        out[used++] = *text++;
    }
    out[used] = 0;
    return used;
}

/* /mnt/sandbox/<title>_<NNN> and its suffix; 0 when it does not fit. */
static int sandbox_path(char *out, size_t size, const char *title, unsigned number, const char *suffix)
{
    char digits[5] = { '_', (char)('0' + number / 100 % 10), (char)('0' + number / 10 % 10), (char)('0' + number % 10), 0 };
    size_t used = append(out, size, 0, "/mnt/sandbox/");

    used = used < size ? append(out, size, used, title) : size;
    used = used < size ? append(out, size, used, digits) : size;
    used = used < size ? append(out, size, used, suffix) : size;
    return used < size;
}

unsigned pw_title_sandbox_find(const char *title, PwTitleSandboxProbe probe, void *context,
                               char *app0, size_t app0_size)
{
    unsigned found = 0, best = 0;
    uint64_t newest = 0;

    for (unsigned number = 0; number < PW_TITLE_SANDBOX_MAX; number++) {
        char dir[PW_TITLE_SANDBOX_PATH_SIZE], eboot[PW_TITLE_SANDBOX_PATH_SIZE];
        uint64_t made = 0;

        if (!sandbox_path(dir, sizeof(dir), title, number, "") ||
            !sandbox_path(eboot, sizeof(eboot), title, number, "/app0/eboot.bin") ||
            probe(dir, eboot, &made, context) != 0)
            continue;
        if (!found || made >= newest) { newest = made; best = number; }
        found++;
    }
    if (!sandbox_path(app0, app0_size, title, best, "/app0") && app0_size) app0[0] = 0;
    return found;
}
