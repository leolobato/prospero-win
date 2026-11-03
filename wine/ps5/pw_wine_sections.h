/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_SECTIONS_H
#define PW_WINE_SECTIONS_H
#include <stddef.h>
#include <stdint.h>

/* Small anonymous sections in direct memory instead of flexible memory.
 *
 * A section's views are shared memory objects mapped from flexible memory, of
 * which a title has little. Chromium keeps a 2 MiB section for each resource it
 * is loading, so a game that loads hundreds at once runs out and its loads fail.
 * A title is one process, so a program's section is never shared with another:
 * the same direct memory mapped at each view's address gives the same result. The
 * Wine server's own sections are not here: it maps them from their fds itself.
 *
 * This module is the bookkeeping: which sections there are, the direct memory
 * behind each, and its views. Direct memory is taken on first use and given back
 * once the section is closed and its last view is gone, in either order, because
 * programs close a section's handle while its view stays in use. A section has one
 * backing for life. The kernel is reached only through the ops, so a host test
 * stands in for the console. Nothing allocates: tables are caller storage. */

typedef struct PwWineSectionOps {
    void *context;
    size_t page;
    int (*allocate)(void *context, size_t bytes, int64_t *offset);
    int (*release)(void *context, int64_t offset, size_t bytes);
    /* Map direct memory read-write where the kernel chooses; *address gets it. */
    int (*map_home)(void *context, size_t bytes, int64_t offset, uintptr_t *address);
    int (*unmap_home)(void *context, uintptr_t address, size_t bytes);
} PwWineSectionOps;

typedef struct PwWineSection {
    uint64_t device, inode;
    size_t bytes;
    int64_t offset;
    uintptr_t home; /* where the module itself reads the section; 0 before first use */
    uint32_t views;
    uint8_t used, closed;
} PwWineSection;

typedef struct PwWineSectionView {
    uintptr_t address;
    size_t bytes;
    uint32_t section;
} PwWineSectionView;

typedef struct PwWineSectionStats {
    uint64_t backed_bytes, peak_backed_bytes, refused;
    uint32_t backed_sections, peak_backed_sections;
} PwWineSectionStats;

typedef struct PwWineSections {
    PwWineSectionOps ops;
    int lock;
    PwWineSection *sections;
    uint32_t section_capacity;
    PwWineSectionView *views;
    uint32_t view_count, view_capacity;
    uintptr_t views_low, views_high;
    PwWineSectionStats stats;
} PwWineSections;

enum {
    PW_WINE_SECTION_NOT_OURS = 0,
    PW_WINE_SECTION_OURS = 1,
    PW_WINE_SECTION_REFUSED = -1,
};

void pw_wine_sections_init(PwWineSections *s, const PwWineSectionOps *ops, PwWineSection *sections,
                           uint32_t section_capacity, PwWineSectionView *views, uint32_t view_capacity);
/* A new section, known by its fd's device and inode, or NOT_OURS when the table is full: the caller gives that one a
 * shared memory object. A closed section is not found again, because its inode may come back for a new one. */
int pw_wine_sections_add(PwWineSections *s, uint64_t device, uint64_t inode, size_t bytes);
/* The last handle to it closed. */
void pw_wine_sections_close(PwWineSections *s, uint64_t device, uint64_t inode);
/* For a view of [position, +bytes): OURS with *offset the direct memory to map there, which the caller then
 * reports with pw_wine_sections_view; on first use it is taken and zeroed. NOT_OURS: not a section here. REFUSED: past
 * the section's end, no room to record a view, or no direct memory left. A section here has nothing else behind it. */
int pw_wine_sections_resolve(PwWineSections *s, uint64_t device, uint64_t inode, uint64_t position, size_t bytes,
                             int64_t *offset);
int pw_wine_sections_view(PwWineSections *s, uint64_t device, uint64_t inode, uintptr_t address, size_t bytes);
/* These addresses no longer hold what they did: views there end, or shrink if only a part went. */
void pw_wine_sections_unview(PwWineSections *s, uintptr_t address, size_t bytes);
/* Copies from a section, for a copy-on-write view. REFUSED when past its end. */
int pw_wine_sections_read(PwWineSections *s, uint64_t device, uint64_t inode, void *to, size_t bytes, uint64_t position);
void pw_wine_sections_stats(PwWineSections *s, PwWineSectionStats *out);

#endif
