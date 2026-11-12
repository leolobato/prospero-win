/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_STEAM_VIEW_H
#define PROSPERO_WIN_PW_STEAM_VIEW_H
/*
 * A Steam game seen where a Steam library keeps it.
 *
 * Some games choose how they start from their own path: The Talos Principle
 * keeps its Steam features only when its program is below steamapps\common.
 * A profile's [steam] install_dir names the game's folder there, and the title
 * shows Wine the game at
 *
 *     C:\Program Files (x86)\Steam\steamapps\common\<install_dir>
 *
 * through a virtual link (wine/ps5/pw_wine_cwd.h) in that folder of the
 * prefix, pointing at the game's real folder. No file moves. The profile keeps
 * the real Z:\ paths, which admission checks; only the program and working
 * directory handed to Wine take the Steam form, and a program a launcher
 * starts (the exec chain) is mapped back before the title checks it.
 */
#include <stddef.h>

#define PW_STEAM_COMMON_WINDOWS "C:\\Program Files (x86)\\Steam\\steamapps\\common"
#define PW_STEAM_COMMON_PREFIX "drive_c/Program Files (x86)/Steam/steamapps/common"

enum { PW_STEAM_INSTALL_DIR_CAPACITY = 129, PW_STEAM_TABLE_MAX = 4096 };

/* Whether name can be a folder of steamapps\common: one Windows path part,
 * not "." or "..", no control character or any of \ / : * ? " < > |, no
 * trailing space or dot, shorter than PW_STEAM_INSTALL_DIR_CAPACITY. */
int pw_steam_install_dir_valid(const char *name, size_t length);

/* "C:\Program Files (x86)\Steam\steamapps\common\<install_dir>". 0, or -1
 * when it does not fit. */
int pw_steam_view_root(const char *install_dir, char *out, size_t size);

/* A Windows path below from_root (ASCII case ignored), or from_root itself,
 * rewritten below to_root: 1. Any other path is copied unchanged: 0. -1 when
 * the result does not fit (out is then empty). */
int pw_steam_view_map(const char *from_root, const char *to_root, const char *path, char *out, size_t size);

/* The link table of steamapps/common with install_dir pointing at target.
 * The prefix is the game's own, so every name already there is this game's
 * (an earlier install_dir): each is kept, in order, and pointed at target too,
 * so none is left on a drive the game has left. install_dir is added last when
 * new. Its length, or -1 when existing is malformed or the table would exceed
 * size. */
long pw_steam_view_table(const char *existing, size_t existing_length, const char *install_dir,
                         const char *target, char *out, size_t size);

#endif
