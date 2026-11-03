/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_sections.h"
#include <string.h>

static void lock(PwWineSections *s)
{
    while (__atomic_exchange_n(&s->lock, 1, __ATOMIC_ACQUIRE)) {}
}
static void unlock(PwWineSections *s)
{
    __atomic_store_n(&s->lock, 0, __ATOMIC_RELEASE);
}

void pw_wine_sections_init(PwWineSections *s, const PwWineSectionOps *ops, PwWineSection *sections,
                           uint32_t section_capacity, PwWineSectionView *views, uint32_t view_capacity)
{
    memset(s, 0, sizeof(*s));
    s->ops = *ops;
    s->sections = sections;
    s->section_capacity = section_capacity;
    s->views = views;
    s->view_capacity = view_capacity;
    memset(sections, 0, section_capacity * sizeof(*sections));
}

static PwWineSection *find(PwWineSections *s, uint64_t device, uint64_t inode)
{
    for (uint32_t i = 0; i < s->section_capacity; i++)
        if (s->sections[i].used && !s->sections[i].closed && s->sections[i].inode == inode &&
            s->sections[i].device == device)
            return &s->sections[i];
    return NULL;
}

static void forget_if_done(PwWineSections *s, PwWineSection *section)
{
    if (!section->closed || section->views) return;
    if (section->home) {
        s->ops.unmap_home(s->ops.context, section->home, section->bytes);
        s->ops.release(s->ops.context, section->offset, section->bytes);
        s->stats.backed_bytes -= section->bytes;
        s->stats.backed_sections--;
    }
    section->used = 0;
}

int pw_wine_sections_add(PwWineSections *s, uint64_t device, uint64_t inode, size_t bytes)
{
    int result = PW_WINE_SECTION_NOT_OURS;

    if (!bytes || bytes % s->ops.page) return result;
    lock(s);
    if (find(s, device, inode)) {
        result = PW_WINE_SECTION_OURS;
    } else {
        for (uint32_t i = 0; i < s->section_capacity; i++) {
            if (s->sections[i].used) continue;
            s->sections[i] = (PwWineSection){ .device = device, .inode = inode, .bytes = bytes, .used = 1 };
            result = PW_WINE_SECTION_OURS;
            break;
        }
        if (result != PW_WINE_SECTION_OURS) s->stats.refused++;
    }
    unlock(s);
    return result;
}

void pw_wine_sections_close(PwWineSections *s, uint64_t device, uint64_t inode)
{
    PwWineSection *section;

    lock(s);
    if ((section = find(s, device, inode))) {
        section->closed = 1;
        forget_if_done(s, section);
    }
    unlock(s);
}

/* Direct memory behind a section, zeroed, on first use. */
static int back(PwWineSections *s, PwWineSection *section)
{
    int64_t offset;
    uintptr_t home;

    if (section->home) return 0;
    if (s->ops.allocate(s->ops.context, section->bytes, &offset)) return -1;
    if (s->ops.map_home(s->ops.context, section->bytes, offset, &home)) {
        s->ops.release(s->ops.context, offset, section->bytes);
        return -1;
    }
    memset((void *)home, 0, section->bytes);
    section->offset = offset;
    section->home = home;
    s->stats.backed_bytes += section->bytes;
    if (s->stats.backed_bytes > s->stats.peak_backed_bytes) s->stats.peak_backed_bytes = s->stats.backed_bytes;
    if (++s->stats.backed_sections > s->stats.peak_backed_sections)
        s->stats.peak_backed_sections = s->stats.backed_sections;
    return 0;
}

int pw_wine_sections_resolve(PwWineSections *s, uint64_t device, uint64_t inode, uint64_t position, size_t bytes,
                             int64_t *offset)
{
    PwWineSection *section;
    int result = PW_WINE_SECTION_NOT_OURS;

    lock(s);
    if ((section = find(s, device, inode))) {
        if (position > section->bytes || bytes > section->bytes - position ||
            s->view_count == s->view_capacity || back(s, section)) {
            s->stats.refused++;
            result = PW_WINE_SECTION_REFUSED;
        } else {
            *offset = section->offset + (int64_t)position;
            result = PW_WINE_SECTION_OURS;
        }
    }
    unlock(s);
    return result;
}

int pw_wine_sections_view(PwWineSections *s, uint64_t device, uint64_t inode, uintptr_t address, size_t bytes)
{
    PwWineSection *section;
    int result = PW_WINE_SECTION_REFUSED;

    lock(s);
    if ((section = find(s, device, inode)) && s->view_count < s->view_capacity) {
        s->views[s->view_count++] = (PwWineSectionView){ address, bytes, (uint32_t)(section - s->sections) };
        section->views++;
        if (!s->views_low || address < s->views_low) s->views_low = address;
        if (address + bytes > s->views_high) s->views_high = address + bytes;
        result = PW_WINE_SECTION_OURS;
    }
    unlock(s);
    return result;
}

void pw_wine_sections_unview(PwWineSections *s, uintptr_t address, size_t bytes)
{
    const uintptr_t high = address + bytes;

    lock(s);
    if (s->view_count && address < s->views_high && high > s->views_low) {
        for (uint32_t i = 0; i < s->view_count;) {
            PwWineSectionView *view = &s->views[i];
            const uintptr_t view_high = view->address + view->bytes;

            if (high <= view->address || address >= view_high) {
                i++;
            } else if (address <= view->address && high >= view_high) {
                PwWineSection *section = &s->sections[view->section];

                *view = s->views[--s->view_count];
                section->views--;
                forget_if_done(s, section);
            } else if (address <= view->address) {
                view->bytes = view_high - high;
                view->address = high;
                i++;
            } else if (high >= view_high) {
                view->bytes = address - view->address;
                i++;
            } else {
                /* A hole in the middle. With no room to record the far part, the section keeps a
                 * reference nothing will drop: its direct memory is lost rather than given back
                 * while that part is still mapped. */
                if (s->view_count < s->view_capacity)
                    s->views[s->view_count++] = (PwWineSectionView){ high, view_high - high, view->section };
                s->sections[view->section].views++;
                view->bytes = address - view->address;
                i++;
            }
        }
        if (!s->view_count) s->views_low = s->views_high = 0;
    }
    unlock(s);
}

int pw_wine_sections_read(PwWineSections *s, uint64_t device, uint64_t inode, void *to, size_t bytes, uint64_t position)
{
    PwWineSection *section;
    int result = PW_WINE_SECTION_NOT_OURS;

    lock(s);
    if ((section = find(s, device, inode))) {
        if (position > section->bytes || bytes > section->bytes - position) {
            result = PW_WINE_SECTION_REFUSED;
        } else {
            if (section->home) memcpy(to, (const char *)section->home + position, bytes);
            else memset(to, 0, bytes);
            result = PW_WINE_SECTION_OURS;
        }
    }
    unlock(s);
    return result;
}

void pw_wine_sections_stats(PwWineSections *s, PwWineSectionStats *out)
{
    lock(s);
    *out = s->stats;
    unlock(s);
}
