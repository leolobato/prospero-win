/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_HELPER_CHANNEL_H
#define PW_HELPER_CHANNEL_H
/*
 * How a [helper]'s program reaches the player while the game runs
 * (native/pw_helper_ps5.c), through files beside its ready file. The title
 * makes them as the helper starts and the game runs, so the program learns
 * what the title can do from which of them are there:
 *
 *   <ready>.say     There, empty, when the title shows the program's
 *                   messages. The program adds lines; the title shows each
 *                   whole line as a notification, at most PW_HELPER_SAY_BURST
 *                   in any PW_HELPER_SAY_WINDOW_MS, and drops the rest.
 *   <ready>.answer  There, empty, once the title can also ask the player.
 *                   The program asks by writing <ready>.ask whole (another
 *                   name first, then a rename): one line,
 *                   "<id> <seconds> <text>". The title shows the text and,
 *                   for that many seconds, takes L3+R3 as the answer
 *                   (src/pw_invite_prompt.h). It then writes "<id> accepted"
 *                   or "<id> expired" to <ready>.answer the same way. A
 *                   question with a new id replaces an open one, which gets
 *                   no answer.
 *
 * The title removes <ready>.ask and <ready>.answer as the helper starts, so
 * an answer never belongs to an earlier run. Nothing here knows which helper
 * runs.
 */
#include <stddef.h>
#include <stdint.h>

#include "pw_helper_notify.h"

enum { PW_HELPER_SAY_BURST = 4, PW_HELPER_QUESTION_MAX_SECONDS = 60 };
#define PW_HELPER_SAY_WINDOW_MS 10000u

typedef struct PwHelperQuestion {
    uint64_t id;                        /* above 0 */
    uint32_t seconds;                   /* 1 to PW_HELPER_QUESTION_MAX_SECONDS */
    char text[PW_HELPER_NOTIFY_TEXT];
} PwHelperQuestion;

/* The question file's first line: 1 with *question filled when it is
 * "<id> <seconds> <text>" with an id above 0, 1 to
 * PW_HELPER_QUESTION_MAX_SECONDS seconds and some text (control characters
 * show as spaces, a long one is cut); 0 and an empty question otherwise. */
int pw_helper_question_parse(const char *text, size_t length, PwHelperQuestion *question);

/* The answer file's text: "<id> accepted\n" or "<id> expired\n". */
void pw_helper_answer_text(uint64_t id, int accepted, char *out, size_t size);

/* Whether one more message (or question) may show at now_ms, a steady
 * clock: at most PW_HELPER_SAY_BURST in any PW_HELPER_SAY_WINDOW_MS. One that
 * may is counted. */
typedef struct PwHelperSayLimit {
    uint64_t sent[PW_HELPER_SAY_BURST];
    unsigned next;
} PwHelperSayLimit;
int pw_helper_say_allowed(PwHelperSayLimit *limit, uint64_t now_ms);

#endif
