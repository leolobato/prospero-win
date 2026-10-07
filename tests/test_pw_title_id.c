/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_title_id.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *answer;
static int answer_rc, calls;
static int fake_source(char *title, size_t size)
{
    calls++;
    if (answer) snprintf(title, size, "%s", answer);
    return answer_rc;
}

static int resolve(const char *kernel, int rc, const char *fallback, char out[PW_TITLE_ID_SIZE])
{
    answer = kernel; answer_rc = rc;
    memset(out, 'x', PW_TITLE_ID_SIZE);
    return pw_title_id_resolve(fake_source, fallback, out);
}

int main(void)
{
    char out[PW_TITLE_ID_SIZE];

    assert(pw_title_id_valid("FAKE78090") && pw_title_id_valid("PPSA99995"));
    assert(!pw_title_id_valid("FAKE7809") && !pw_title_id_valid("FAKE780901"));
    assert(!pw_title_id_valid("fake78090") && !pw_title_id_valid("FAKE7809A"));
    assert(!pw_title_id_valid("") && !pw_title_id_valid(NULL));

    /* The kernel's answer wins over the compiled ID. */
    assert(resolve("FAKE78123", 0, "PPSA99995", out) == 1 && !strcmp(out, "FAKE78123"));
    /* A failed call, or anything that is not a title ID, falls back. */
    assert(resolve("FAKE78123", -1, "PPSA99995", out) == 0 && !strcmp(out, "PPSA99995"));
    assert(resolve("", 0, "PPSA99995", out) == 0 && !strcmp(out, "PPSA99995"));
    assert(resolve("FAKE78123_000", 0, "PPSA99995", out) == 0 && !strcmp(out, "PPSA99995"));
    assert(resolve("NPXS4000", 0, "PPSA99995", out) == 0 && !strcmp(out, "PPSA99995"));
    assert(resolve("../../etc", 0, "PPSA99995", out) == 0 && !strcmp(out, "PPSA99995"));
    /* No source at all: the compiled ID. */
    assert(pw_title_id_resolve(NULL, "FAKE78001", out) == 0 && !strcmp(out, "FAKE78001"));
    /* Neither valid: empty and refused. */
    assert(resolve(NULL, -1, "bad", out) == -1 && out[0] == '\0');
    puts("title id: kernel answer, fallback and refusal covered");
    return 0;
}
