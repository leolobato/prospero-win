/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_title_id.h"
#include <string.h>

int pw_title_id_valid(const char *text)
{
    if (!text) return 0;
    for (int i = 0; i < 9; i++) {
        char c = text[i];
        if (i < 4 ? c < 'A' || c > 'Z' : c < '0' || c > '9') return 0;
    }
    return text[9] == '\0';
}

int pw_title_id_resolve(PwTitleIdSource source, const char *fallback,
                        char out[PW_TITLE_ID_SIZE])
{
    char found[32];

    memset(found, 0, sizeof(found));
    if (source && source(found, sizeof(found)) == 0 && pw_title_id_valid(found)) {
        memcpy(out, found, PW_TITLE_ID_SIZE);
        return 1;
    }
    if (pw_title_id_valid(fallback)) {
        memcpy(out, fallback, PW_TITLE_ID_SIZE);
        return 0;
    }
    out[0] = '\0';
    return -1;
}
