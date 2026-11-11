/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_helper_channel.h"

#include <stdio.h>
#include <string.h>

/* Decimal digits at text[*at, length) up to max: 1 with *value and *at past
 * them, 0 when there are none or the number is above max. */
static int number(const char *text, size_t length, size_t *at, uint64_t max, uint64_t *value)
{
    const size_t start = *at;
    uint64_t v = 0;

    while (*at < length && text[*at] >= '0' && text[*at] <= '9') {
        const unsigned digit = (unsigned)(text[*at] - '0');

        if (v > (max - digit) / 10u) return 0;
        v = v * 10u + digit;
        (*at)++;
    }
    if (*at == start) return 0;
    *value = v;
    return 1;
}

int pw_helper_question_parse(const char *text, size_t length, PwHelperQuestion *question)
{
    size_t end = 0, at = 0, n = 0;
    uint64_t id = 0, seconds = 0;
    int visible = 0;

    memset(question, 0, sizeof(*question));
    while (end < length && text[end] != '\n' && text[end] != '\r') end++;
    if (!number(text, end, &at, UINT64_MAX, &id) || !id || at >= end || text[at] != ' ') return 0;
    at++;
    if (!number(text, end, &at, PW_HELPER_QUESTION_MAX_SECONDS, &seconds) || !seconds || at >= end || text[at] != ' ')
        return 0;
    for (at++; at < end && n + 1 < sizeof(question->text); at++) {
        const char c = (unsigned char)text[at] < 0x20u || text[at] == 0x7f ? ' ' : text[at];

        visible |= c != ' ';
        question->text[n++] = c;
    }
    if (!visible) {
        memset(question, 0, sizeof(*question));
        return 0;
    }
    question->id = id;
    question->seconds = (uint32_t)seconds;
    return 1;
}

void pw_helper_answer_text(uint64_t id, int accepted, char *out, size_t size)
{
    if (size) snprintf(out, size, "%llu %s\n", (unsigned long long)id, accepted ? "accepted" : "expired");
}

int pw_helper_say_allowed(PwHelperSayLimit *limit, uint64_t now_ms)
{
    uint64_t *oldest = &limit->sent[limit->next % PW_HELPER_SAY_BURST];
    const uint64_t stamp = now_ms + 1u;   /* 0 is a slot never used */

    if (*oldest && stamp - *oldest < PW_HELPER_SAY_WINDOW_MS) return 0;
    *oldest = stamp;
    limit->next = (limit->next + 1u) % PW_HELPER_SAY_BURST;
    return 1;
}
