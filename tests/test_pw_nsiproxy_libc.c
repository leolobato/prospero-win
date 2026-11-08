/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* wine/ps5/pw_nsiproxy_libc.c is included with its names prefixed, so the
 * C library's own if_nameindex stays the reference for the names. */
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

struct if_nameindex *pw_test_if_nameindex(void);
void pw_test_if_freenameindex(struct if_nameindex *list);
unsigned int pw_test_if_nametoindex(const char *name);
#define if_nameindex(...) pw_test_if_nameindex(__VA_ARGS__)
#define if_freenameindex(...) pw_test_if_freenameindex(__VA_ARGS__)
#define if_nametoindex(...) pw_test_if_nametoindex(__VA_ARGS__)
#include "../wine/ps5/pw_nsiproxy_libc.c"
#undef if_nameindex
#undef if_freenameindex
#undef if_nametoindex

int main(void)
{
    struct if_nameindex *ours = pw_test_if_nameindex(), *theirs = if_nameindex(), *a, *b;
    size_t count = 0;

    assert(ours && theirs);
    /* The same interfaces, each name once, each with an index. */
    for (a = theirs; a->if_name; a++)
    {
        size_t found = 0;
        for (b = ours; b->if_name; b++)
            if (!strcmp(a->if_name, b->if_name))
            {
                found++;
                assert(b->if_index);
            }
        assert(found == 1);
        assert(pw_test_if_nametoindex(a->if_name));
    }
    for (b = ours; b->if_name; b++) count++;
    for (a = theirs; a->if_name; a++) count--;
    assert(count == 0);
    /* Indices are unique. */
    for (a = ours; a->if_name; a++)
        for (b = a + 1; b->if_name; b++) assert(a->if_index != b->if_index);

    /* An entry without an index never takes one a real interface has. */
    {
        struct if_nameindex list[4] = { { 0, "a" }, { 1, "b" }, { 0, "c" }, { 3, "d" } };
        fill_missing_indices(list, 4);
        assert(list[0].if_index == 2 && list[1].if_index == 1 && list[2].if_index == 4 && list[3].if_index == 3);
    }

    errno = 0;
    assert(!pw_test_if_nametoindex("pw-no-such-interface"));
    assert(errno == ENXIO);
    pw_test_if_freenameindex(NULL);
    pw_test_if_freenameindex(ours);
    if_freenameindex(theirs);
    printf("test_pw_nsiproxy_libc: ok\n");
    return 0;
}
