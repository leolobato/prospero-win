/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_helper_channel.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int parse(const char *text, PwHelperQuestion *question)
{
    return pw_helper_question_parse(text, strlen(text), question);
}

int main(void)
{
    PwHelperQuestion question;
    char out[64];

    /* A question: its id, how long the player has, and what to show. Only the first line counts. */
    assert(parse("7 15 Mara invited you. Press L3 + R3 to join.\n", &question));
    assert(question.id == 7 && question.seconds == 15 && !strcmp(question.text, "Mara invited you. Press L3 + R3 to join."));
    assert(parse("18446744073709551615 60 x\r\nmore", &question) && question.id == 18446744073709551615ull &&
           question.seconds == 60 && !strcmp(question.text, "x"));
    assert(parse("1 1 a\tb", &question) && !strcmp(question.text, "a b"));

    /* Anything else is no question, and leaves none behind. */
    {
        static const char *const bad[] = {
            "", "\n", "7", "7 15", "7 15 ", "7 15    ", "7 15 \t", "0 15 text", "7 0 text", "7 61 text",
            "18446744073709551616 15 text", "-7 15 text", "7 -15 text", " 7 15 text", "7  15 text", "7x 15 text",
            "7 15x text", "7\t15 text", "\n7 15 text",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            memset(&question, 0x5a, sizeof(question));
            assert(!parse(bad[i], &question));
            assert(!question.id && !question.seconds && !question.text[0]);
        }
    }
    /* A long text is cut to fit, still a question. */
    {
        char line[600];
        memset(line, 'a', sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;
        memcpy(line, "3 9 ", 4);
        assert(parse(line, &question) && strlen(question.text) == PW_HELPER_NOTIFY_TEXT - 1);
    }

    /* The answer. */
    pw_helper_answer_text(7, 1, out, sizeof(out));
    assert(!strcmp(out, "7 accepted\n"));
    pw_helper_answer_text(18446744073709551615ull, 0, out, sizeof(out));
    assert(!strcmp(out, "18446744073709551615 expired\n"));

    /* Four messages in any ten seconds; the fifth waits until the first is ten seconds old. */
    {
        PwHelperSayLimit limit;
        memset(&limit, 0, sizeof(limit));
        assert(pw_helper_say_allowed(&limit, 1000) && pw_helper_say_allowed(&limit, 1100) &&
               pw_helper_say_allowed(&limit, 1200) && pw_helper_say_allowed(&limit, 1300));
        assert(!pw_helper_say_allowed(&limit, 1400) && !pw_helper_say_allowed(&limit, 10999));
        assert(pw_helper_say_allowed(&limit, 11000));
        assert(!pw_helper_say_allowed(&limit, 11050));
        assert(pw_helper_say_allowed(&limit, 11100));
        /* A clock at 0 still counts. */
        memset(&limit, 0, sizeof(limit));
        for (int i = 0; i < PW_HELPER_SAY_BURST; i++) assert(pw_helper_say_allowed(&limit, 0));
        assert(!pw_helper_say_allowed(&limit, 0));
    }
    puts("test_pw_helper_channel: ok");
    return 0;
}
