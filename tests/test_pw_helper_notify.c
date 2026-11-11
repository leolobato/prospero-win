/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_helper_notify.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char out[PW_HELPER_NOTIFY_TEXT], reason[PW_HELPER_NOTIFY_TEXT], name[PW_HELPER_NOTIFY_NAME];
    size_t at = 0;

    /* The program names the helper. */
    pw_helper_display_name("Z:\\data\\x\\client\\steam.exe", name, sizeof(name));
    assert(!strcmp(name, "Steam"));
    pw_helper_display_name("/opt/helper/Overlay", name, sizeof(name));
    assert(!strcmp(name, "Overlay"));
    pw_helper_display_name("C:\\a\\", name, sizeof(name));
    assert(!strcmp(name, "The helper"));
    pw_helper_display_name(NULL, name, sizeof(name));
    assert(!strcmp(name, "The helper"));
    pw_helper_display_name("C:\\.hidden", name, sizeof(name));
    assert(!strcmp(name, ".hidden"));

    /* Why it didn't start, for the player. */
    pw_helper_failure_text("Steam", "sign-in timed out", out, sizeof(out));
    assert(!strcmp(out, "Steam didn't start: sign-in timed out. The game runs without it; close the game and try again."));
    pw_helper_failure_text("Steam", "", out, sizeof(out));
    assert(!strcmp(out, "Steam didn't start. The game runs without it; close the game and try again."));
    pw_helper_failure_text("Steam", "a\tb", out, sizeof(out));
    assert(strstr(out, ": a b."));

    /* The ready file: "failed <reason>" is a failure; anything else is ready. */
    assert(pw_helper_ready_failed("failed sign-in timed out\n", 25, reason, sizeof(reason)) && !strcmp(reason, "sign-in timed out"));
    assert(pw_helper_ready_failed("failed", 6, reason, sizeof(reason)) && !reason[0]);
    assert(!pw_helper_ready_failed("ok\n", 3, reason, sizeof(reason)) && !reason[0]);
    assert(!pw_helper_ready_failed("failedx", 7, reason, sizeof(reason)));
    assert(!pw_helper_ready_failed("", 0, reason, sizeof(reason)));

    /* Lines the program adds: whole ones only, empty ones skipped, a partial one waits. */
    {
        const char text[] = "Steam: client loaded\n\nSteam: signing in\r\nSteam: sig";
        assert(pw_helper_next_line(text, sizeof(text) - 1, &at, out, sizeof(out)) && !strcmp(out, "Steam: client loaded"));
        assert(pw_helper_next_line(text, sizeof(text) - 1, &at, out, sizeof(out)) && !strcmp(out, "Steam: signing in"));
        assert(!pw_helper_next_line(text, sizeof(text) - 1, &at, out, sizeof(out)));
        assert(at == strlen("Steam: client loaded\n\nSteam: signing in\r\n"));
    }
    puts("test_pw_helper_notify: ok");
    return 0;
}
