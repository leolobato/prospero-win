/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_helper_notify.h"

#include <string.h>

/* Adds text to out (NUL-terminated, size bytes), cut to fit. */
static void append(char *out, size_t size, const char *text)
{
    size_t n = strlen(out);

    while (*text && n + 1 < size) out[n++] = *text++;
    out[n] = 0;
}

static void copy_clean(char *out, size_t size, const char *text, size_t length)
{
    size_t n = 0;

    if (!size) return;
    for (size_t i = 0; i < length && text[i] && n + 1 < size; i++)
        out[n++] = (unsigned char)text[i] < 0x20u || text[i] == 0x7f ? ' ' : text[i];
    out[n] = 0;
}

void pw_helper_display_name(const char *program, char *out, size_t size)
{
    const char *base = program ? program : "";
    const char *dot;
    size_t length;

    if (!size) return;
    for (const char *at = base; *at; at++)
        if (*at == '\\' || *at == '/') base = at + 1;
    dot = strrchr(base, '.');
    length = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    copy_clean(out, size, base, length);
    if (!out[0]) {
        append(out, size, "The helper");
        return;
    }
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}

void pw_helper_failure_text(const char *name, const char *reason, char *out, size_t size)
{
    char clean[PW_HELPER_NOTIFY_TEXT];

    if (!size) return;
    copy_clean(clean, sizeof(clean), reason ? reason : "", reason ? strlen(reason) : 0);
    out[0] = 0;
    append(out, size, name);
    append(out, size, " didn't start");
    if (clean[0]) {
        append(out, size, ": ");
        append(out, size, clean);
    }
    append(out, size, ". The game runs without it; close the game and try again.");
}

int pw_helper_ready_failed(const char *text, size_t length, char *reason, size_t size)
{
    size_t end = 0;

    if (size) reason[0] = 0;
    while (end < length && text[end] != '\n' && text[end] != '\r') end++;
    if (end < 6 || memcmp(text, "failed", 6) != 0 || (end > 6 && text[6] != ' ')) return 0;
    if (end > 7) copy_clean(reason, size, text + 7, end - 7);
    return 1;
}

int pw_helper_next_line(const char *buffer, size_t used, size_t *at, char *out, size_t size)
{
    while (*at < used) {
        const char *end = memchr(buffer + *at, '\n', used - *at);
        size_t length;

        if (!end) return 0;
        length = (size_t)(end - (buffer + *at));
        if (length && buffer[*at + length - 1] == '\r') length--;
        if (!length) {
            *at = (size_t)(end - buffer) + 1;
            continue;
        }
        copy_clean(out, size, buffer + *at, length);
        *at = (size_t)(end - buffer) + 1;
        return 1;
    }
    return 0;
}
