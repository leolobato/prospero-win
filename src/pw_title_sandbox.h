/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_TITLE_SANDBOX_H
#define PROSPERO_WIN_PW_TITLE_SANDBOX_H
#include <stddef.h>
#include <stdint.h>

/* Where a title's files are visible once /data is granted, and /app0 is not:
 * /mnt/sandbox/<title>_<NNN>/app0. The console numbers a title's sandboxes
 * from _000 and takes the next one while an earlier one is still mounted, so
 * a start after a sandbox that did not go away runs from _001 or later. */
enum {
    PW_TITLE_SANDBOX_MAX = 256,
    PW_TITLE_SANDBOX_PATH_SIZE = sizeof("/mnt/sandbox/ABCD12345_000/app0/eboot.bin") + 8
};

/* Reports whether the sandbox folder dir holds a title's eboot at eboot, and
 * when dir was made; 0 when it does. */
typedef int (*PwTitleSandboxProbe)(const char *dir, const char *eboot, uint64_t *made, void *context);

/* Writes the app0 path of the newest sandbox of title that has its eboot into
 * app0: the higher number when two were made at the same moment. Returns how
 * many sandboxes have an eboot; with none, app0 names _000. */
unsigned pw_title_sandbox_find(const char *title, PwTitleSandboxProbe probe, void *context,
                               char *app0, size_t app0_size);
#endif
