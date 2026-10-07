/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_TITLE_ID_H
#define PROSPERO_WIN_PW_TITLE_ID_H
#include <stddef.h>

/* The title ID this process runs as. One shell binary can serve every title a
 * launcher makes on the console, so the ID is asked of the kernel at startup
 * (sceKernelGetAppInfo of this process, measured before and after the /data
 * grant); the compiled ID is the fallback. */
enum { PW_TITLE_ID_SIZE = 10 };   /* four letters, five digits, NUL */

/* Writes this process's title ID into title (NUL-terminated within size);
 * 0 on success. */
typedef int (*PwTitleIdSource)(char *title, size_t size);

/* Non-zero for four uppercase letters followed by five digits. */
int pw_title_id_valid(const char *text);

/* Copies the source's ID into out when it gives a valid one, else fallback.
 * Returns 1 when the ID came from the source, 0 for the fallback, -1 when
 * neither is valid (out is then empty). */
int pw_title_id_resolve(PwTitleIdSource source, const char *fallback,
                        char out[PW_TITLE_ID_SIZE]);
#endif
