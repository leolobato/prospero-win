/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_title_sandbox.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* made[n] is when sandbox n was made; 0 means it has no eboot. */
static int probe(const char *dir, const char *eboot, uint64_t *made, void *context)
{
    const uint64_t *table = context;
    unsigned number = 999;

    assert(sscanf(dir, "/mnt/sandbox/FAKE78001_%u", &number) == 1 && number < PW_TITLE_SANDBOX_MAX);
    assert(strstr(eboot, dir) == eboot && !strcmp(eboot + strlen(dir), "/app0/eboot.bin"));
    if (!table[number]) return -1;
    *made = table[number];
    return 0;
}

static unsigned find(const uint64_t *table, char out[PW_TITLE_SANDBOX_PATH_SIZE])
{
    return pw_title_sandbox_find("FAKE78001", probe, (void *)table, out, PW_TITLE_SANDBOX_PATH_SIZE);
}

int main(void)
{
    char out[PW_TITLE_SANDBOX_PATH_SIZE];
    uint64_t table[PW_TITLE_SANDBOX_MAX] = { 0 };

    /* No sandbox answers: the path every start used before. */
    assert(find(table, out) == 0 && !strcmp(out, "/mnt/sandbox/FAKE78001_000/app0"));
    /* The usual start. */
    table[0] = 5;
    assert(find(table, out) == 1 && !strcmp(out, "/mnt/sandbox/FAKE78001_000/app0"));
    /* _000 lost its files, the title runs in _001. */
    table[0] = 0; table[1] = 5;
    assert(find(table, out) == 1 && !strcmp(out, "/mnt/sandbox/FAKE78001_001/app0"));
    /* _000 is left over with its files, the newer one is ours. */
    table[0] = 5; table[1] = 9;
    assert(find(table, out) == 2 && !strcmp(out, "/mnt/sandbox/FAKE78001_001/app0"));
    /* _001 is left over and the console went back to _000. */
    table[0] = 9; table[1] = 5;
    assert(find(table, out) == 2 && !strcmp(out, "/mnt/sandbox/FAKE78001_000/app0"));
    /* A gap, a tie and the last number. */
    table[0] = 0; table[1] = 0; table[7] = 9; table[PW_TITLE_SANDBOX_MAX - 1] = 9;
    assert(find(table, out) == 2 && !strcmp(out, "/mnt/sandbox/FAKE78001_255/app0"));
    puts("test_pw_title_sandbox: ok");
    return 0;
}
