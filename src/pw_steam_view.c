/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_steam_view.h"

#include <stdio.h>
#include <string.h>

int pw_steam_install_dir_valid(const char *name, size_t length)
{
    if (!name || !length || length >= PW_STEAM_INSTALL_DIR_CAPACITY) return 0;
    if ((length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
    if (name[length - 1] == ' ' || name[length - 1] == '.') return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || strchr("\\/:*?\"<>|", c)) return 0;
    }
    return 1;
}

int pw_steam_view_root(const char *install_dir, char *out, size_t size)
{
    int used;

    if (!install_dir || !out || !size || !pw_steam_install_dir_valid(install_dir, strlen(install_dir))) return -1;
    used = snprintf(out, size, "%s\\%s", PW_STEAM_COMMON_WINDOWS, install_dir);
    if (used < 0 || (size_t)used >= size) {
        out[0] = 0;
        return -1;
    }
    return 0;
}

static int ascii_lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

int pw_steam_view_map(const char *from_root, const char *to_root, const char *path, char *out, size_t size)
{
    size_t from = from_root ? strlen(from_root) : 0, i;
    int used;

    if (!from_root || !to_root || !path || !out || !size) return -1;
    for (i = 0; i < from && path[i] && ascii_lower((unsigned char)path[i]) == ascii_lower((unsigned char)from_root[i]); i++) {}
    if (!from || i < from || (path[from] && path[from] != '\\')) {
        used = snprintf(out, size, "%s", path);
        if (used < 0 || (size_t)used >= size) { out[0] = 0; return -1; }
        return 0;
    }
    used = snprintf(out, size, "%s%s", to_root, path + from);
    if (used < 0 || (size_t)used >= size) { out[0] = 0; return -1; }
    return 1;
}

long pw_steam_view_table(const char *existing, size_t existing_length, const char *install_dir,
                         const char *target, char *out, size_t size)
{
    size_t used = 0, name_length;
    int found = 0;
    const char *line = existing, *end = existing ? existing + existing_length : NULL;
    int written;

    if (!install_dir || !target || !target[0] || !out || !size || strchr(target, '\n') || strchr(target, '\t') ||
        !pw_steam_install_dir_valid(install_dir, strlen(install_dir)) || (existing_length && !existing))
        return -1;
    name_length = strlen(install_dir);
    while (line && line < end) {
        const char *newline = memchr(line, '\n', (size_t)(end - line)), *next;
        const char *tab;
        size_t length;

        next = newline ? newline + 1 : end;
        length = (size_t)((newline ? newline : end) - line);
        if (length) {
            tab = memchr(line, '\t', length);
            if (!tab || tab == line || tab + 1 == line + length) return -1;
            size_t name = (size_t)(tab - line);
            if (name == name_length && !memcmp(line, install_dir, name_length)) found = 1;
            written = snprintf(out + used, size - used, "%.*s\t%s\n", (int)name, line, target);
            if (written < 0 || (size_t)written >= size - used) return -1;
            used += (size_t)written;
        }
        line = next;
    }
    if (found) return (long)used;
    written = snprintf(out + used, size - used, "%s\t%s\n", install_dir, target);
    if (written < 0 || (size_t)written >= size - used) return -1;
    return (long)(used + (size_t)written);
}
