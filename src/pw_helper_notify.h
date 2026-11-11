/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_HELPER_NOTIFY_H
#define PW_HELPER_NOTIFY_H
/*
 * What the title shows about its [helper] while the game waits for it
 * (native/pw_helper_ps5.c): the program's start, step by step, when a notify
 * file is beside its ready file, and why it did not get ready, always. The
 * text names the helper by its program, so nothing here knows which helper
 * runs.
 */
#include <stddef.h>

enum { PW_HELPER_NOTIFY_NAME = 64, PW_HELPER_NOTIFY_TEXT = 256 };

/* The helper's name for the player: its program's file name without the
 * extension, the first letter upper case ("Z:\\a\\steam.exe" -> "Steam").
 * "The helper" when the program has no usable name. */
void pw_helper_display_name(const char *program, char *out, size_t size);

/* "<name> didn't start: <reason>. The game runs without it; close the game
 * and try again." Control characters in reason show as spaces. */
void pw_helper_failure_text(const char *name, const char *reason, char *out, size_t size);

/* A ready file's first line: 1 and its reason when it is "failed <reason>",
 * 0 for anything else (the program is ready). */
int pw_helper_ready_failed(const char *text, size_t length, char *reason, size_t size);

/* The next complete line ('\n'-terminated) in buffer[*at, used): 1 with the
 * line in out (without its line end, cut to fit) and *at past it; 0 when no
 * whole line is left. Empty lines are skipped. */
int pw_helper_next_line(const char *buffer, size_t used, size_t *at, char *out, size_t size);

#endif
