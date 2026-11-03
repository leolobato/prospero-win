/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/pw_wine_sections.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE = 0x4000 };
static int allocations, releases, maps, unmaps, refuse_allocate;
static char pool[64 * PAGE];
static size_t pool_used;

static int do_allocate(void *c, size_t bytes, int64_t *offset)
{
    (void)c;
    if (refuse_allocate || pool_used + bytes > sizeof(pool)) return 1;
    memset(pool + pool_used, 0x7e, bytes); /* what direct memory holds before it is cleared */
    *offset = (int64_t)pool_used;
    pool_used += bytes;
    allocations++;
    return 0;
}
static int do_release(void *c, int64_t offset, size_t bytes)
{
    (void)c; (void)offset; (void)bytes;
    releases++;
    return 0;
}
static int do_map_home(void *c, size_t bytes, int64_t offset, uintptr_t *address)
{
    (void)c; (void)bytes;
    *address = (uintptr_t)(pool + offset);
    maps++;
    return 0;
}
static int do_unmap_home(void *c, uintptr_t address, size_t bytes)
{
    (void)c; (void)address; (void)bytes;
    unmaps++;
    return 0;
}

static PwWineSection sections[4];
static PwWineSectionView views[4];
static PwWineSections s;

static void fresh(void)
{
    static const PwWineSectionOps ops = { NULL, PAGE, do_allocate, do_release, do_map_home, do_unmap_home };
    allocations = releases = maps = unmaps = refuse_allocate = 0;
    pool_used = 0;
    pw_wine_sections_init(&s, &ops, sections, 4, views, 4);
}

int main(void)
{
    int64_t offset = -1;
    char bytes[8];
    PwWineSectionStats stats;

    /* Unknown sections and sizes that aren't whole pages aren't ours. */
    fresh();
    assert(pw_wine_sections_resolve(&s, 1, 1, 0, PAGE, &offset) == PW_WINE_SECTION_NOT_OURS);
    assert(pw_wine_sections_add(&s, 1, 1, PAGE + 1) == PW_WINE_SECTION_NOT_OURS);

    /* No direct memory until first use, and it comes zeroed. */
    assert(pw_wine_sections_add(&s, 1, 1, 2 * PAGE) == PW_WINE_SECTION_OURS);
    assert(!allocations);
    assert(pw_wine_sections_read(&s, 1, 1, bytes, sizeof(bytes), 0) == PW_WINE_SECTION_OURS && !bytes[0] && !allocations);
    assert(pw_wine_sections_resolve(&s, 1, 1, PAGE, PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(allocations == 1 && offset == PAGE && pool[0] == 0 && pool[2 * PAGE - 1] == 0);

    /* Past the end is refused; a second view uses the same memory. */
    assert(pw_wine_sections_resolve(&s, 1, 1, PAGE, 2 * PAGE, &offset) == PW_WINE_SECTION_REFUSED);
    assert(pw_wine_sections_resolve(&s, 1, 1, 3 * PAGE, 0, &offset) == PW_WINE_SECTION_REFUSED);
    assert(pw_wine_sections_resolve(&s, 1, 1, 0, 2 * PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(allocations == 1 && offset == 0);

    /* Reads see what views wrote, for copy-on-write views. */
    pool[PAGE + 3] = 'x';
    assert(pw_wine_sections_read(&s, 1, 1, bytes, 1, PAGE + 3) == PW_WINE_SECTION_OURS && bytes[0] == 'x');
    assert(pw_wine_sections_read(&s, 1, 1, bytes, 8, 2 * PAGE) == PW_WINE_SECTION_REFUSED);

    /* Closed with a view alive: the memory stays until the view goes. */
    assert(pw_wine_sections_view(&s, 1, 1, 0x100000, 2 * PAGE) == PW_WINE_SECTION_OURS);
    pw_wine_sections_close(&s, 1, 1);
    assert(!releases);
    pw_wine_sections_unview(&s, 0x200000, PAGE); /* someone else's addresses */
    assert(!releases);
    pw_wine_sections_unview(&s, 0x100000, 2 * PAGE);
    assert(releases == 1 && unmaps == 1);
    assert(pw_wine_sections_resolve(&s, 1, 1, 0, PAGE, &offset) == PW_WINE_SECTION_NOT_OURS);

    /* The view goes first: the memory stays for a later view, until the close. */
    fresh();
    pw_wine_sections_add(&s, 1, 2, PAGE);
    assert(pw_wine_sections_resolve(&s, 1, 2, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(pw_wine_sections_view(&s, 1, 2, 0x100000, PAGE) == PW_WINE_SECTION_OURS);
    pw_wine_sections_unview(&s, 0x100000, PAGE);
    assert(!releases);
    pool[0] = 'k';
    assert(pw_wine_sections_resolve(&s, 1, 2, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(allocations == 1 && pool[0] == 'k');
    pw_wine_sections_close(&s, 1, 2);
    assert(releases == 1);

    /* Closed without ever being used: nothing to give back. */
    fresh();
    pw_wine_sections_add(&s, 1, 3, PAGE);
    pw_wine_sections_close(&s, 1, 3);
    assert(!allocations && !releases);

    /* Parts of a view go: head, tail, a hole. The reference lasts until the last part. */
    fresh();
    pw_wine_sections_add(&s, 1, 4, 8 * PAGE);
    assert(pw_wine_sections_resolve(&s, 1, 4, 0, 8 * PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(pw_wine_sections_view(&s, 1, 4, 0x100000, 8 * PAGE) == PW_WINE_SECTION_OURS);
    pw_wine_sections_close(&s, 1, 4);
    pw_wine_sections_unview(&s, 0x100000, PAGE);
    pw_wine_sections_unview(&s, 0x100000 + 7 * PAGE, PAGE);
    pw_wine_sections_unview(&s, 0x100000 + 3 * PAGE, PAGE);
    assert(s.view_count == 2 && !releases);
    pw_wine_sections_unview(&s, 0x100000 + PAGE, 2 * PAGE);
    assert(s.view_count == 1 && !releases);
    pw_wine_sections_unview(&s, 0x100000 + 4 * PAGE, 3 * PAGE);
    assert(!s.view_count && releases == 1);

    /* No direct memory on first use: refused, and it works once there is some. */
    fresh();
    pw_wine_sections_add(&s, 1, 5, PAGE);
    refuse_allocate = 1;
    assert(pw_wine_sections_resolve(&s, 1, 5, 0, PAGE, &offset) == PW_WINE_SECTION_REFUSED);
    refuse_allocate = 0;
    assert(pw_wine_sections_resolve(&s, 1, 5, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);

    /* Adding a known section again changes nothing. */
    fresh();
    pw_wine_sections_add(&s, 1, 9, PAGE);
    assert(pw_wine_sections_resolve(&s, 1, 9, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
    pool[0] = 'k';
    assert(pw_wine_sections_add(&s, 1, 9, PAGE) == PW_WINE_SECTION_OURS);
    assert(pw_wine_sections_resolve(&s, 1, 9, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(allocations == 1 && pool[0] == 'k');

    /* Closed with a view alive, its inode comes back for a new section: that one has its own memory. */
    fresh();
    pw_wine_sections_add(&s, 1, 10, PAGE);
    assert(pw_wine_sections_resolve(&s, 1, 10, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(pw_wine_sections_view(&s, 1, 10, 0x100000, PAGE) == PW_WINE_SECTION_OURS);
    pool[0] = 'o';
    pw_wine_sections_close(&s, 1, 10);
    assert(!releases);
    assert(pw_wine_sections_add(&s, 1, 10, PAGE) == PW_WINE_SECTION_OURS);
    assert(pw_wine_sections_resolve(&s, 1, 10, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(allocations == 2 && offset == PAGE && pool[0] == 'o' && pool[PAGE] == 0);
    pw_wine_sections_unview(&s, 0x100000, PAGE);
    assert(releases == 1);

    /* A full view table refuses a section already in direct memory, never moves it. */
    fresh();
    pw_wine_sections_add(&s, 1, 6, PAGE);
    for (uintptr_t i = 0; i < 4; i++) {
        assert(pw_wine_sections_resolve(&s, 1, 6, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
        assert(pw_wine_sections_view(&s, 1, 6, 0x100000 + i * PAGE, PAGE) == PW_WINE_SECTION_OURS);
    }
    assert(pw_wine_sections_resolve(&s, 1, 6, 0, PAGE, &offset) == PW_WINE_SECTION_REFUSED);

    /* A hole with no room to record the far part keeps the memory for good: that part may still be mapped. */
    pw_wine_sections_unview(&s, 0x100000 + 3 * PAGE, PAGE);
    fresh();
    pw_wine_sections_add(&s, 1, 7, 4 * PAGE);
    pw_wine_sections_add(&s, 1, 8, PAGE);
    assert(pw_wine_sections_resolve(&s, 1, 7, 0, 4 * PAGE, &offset) == PW_WINE_SECTION_OURS);
    assert(pw_wine_sections_view(&s, 1, 7, 0x100000, 4 * PAGE) == PW_WINE_SECTION_OURS);
    assert(pw_wine_sections_resolve(&s, 1, 8, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
    for (uintptr_t i = 0; i < 3; i++) assert(pw_wine_sections_view(&s, 1, 8, 0x200000 + i * PAGE, PAGE) == PW_WINE_SECTION_OURS);
    pw_wine_sections_close(&s, 1, 7);
    pw_wine_sections_unview(&s, 0x100000 + PAGE, PAGE);
    assert(s.view_count == 4 && !releases);
    pw_wine_sections_unview(&s, 0x100000, PAGE);
    assert(!releases && s.view_count == 3);

    /* A full section table leaves a new one as a shared memory object. */
    fresh();
    for (uint64_t i = 0; i < 4; i++) assert(pw_wine_sections_add(&s, 2, i, PAGE) == PW_WINE_SECTION_OURS);
    assert(pw_wine_sections_add(&s, 2, 9, PAGE) == PW_WINE_SECTION_NOT_OURS);
    pw_wine_sections_stats(&s, &stats);
    assert(stats.refused == 1);
    /* Room again: new ones are taken again. */
    pw_wine_sections_close(&s, 2, 0);
    assert(pw_wine_sections_add(&s, 2, 9, PAGE) == PW_WINE_SECTION_OURS);

    /* Stats follow what is backed. */
    fresh();
    pw_wine_sections_add(&s, 3, 1, PAGE);
    assert(pw_wine_sections_resolve(&s, 3, 1, 0, PAGE, &offset) == PW_WINE_SECTION_OURS);
    pw_wine_sections_close(&s, 3, 1);
    pw_wine_sections_stats(&s, &stats);
    assert(!stats.backed_bytes && stats.peak_backed_bytes == PAGE && stats.peak_backed_sections == 1);

    puts("pw_wine_sections tests passed");
    return 0;
}
