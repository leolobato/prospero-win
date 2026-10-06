/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_TITLE_PUBLICATION_NATIVE_H
#define PROSPERO_WIN_PW_TITLE_PUBLICATION_NATIVE_H

#include "../src/pw_game_profile.h"
#include "../src/pw_title_launch.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PW_TITLE_PUBLICATION_NATIVE_PATH = 1024,
    PW_TITLE_PUBLICATION_ADMISSION_INVALID = -1,
    PW_TITLE_PUBLICATION_ADMISSION_BUSY = -2,
};

typedef struct PwTitlePublicationPlatform {
    /* Test-only namespace prefix. Production callers pass NULL. Canonical
     * policy is always evaluated before this prefix is applied. */
    const char *host_root;
    uint32_t owner_uid;
    /* Must perform the qualified equivalent of flock(fd, LOCK_SH|LOCK_NB).
     * Return zero on acquisition and set errno on failure. */
    int (*try_lock_shared)(void *context, int descriptor);
    void *context;
} PwTitlePublicationPlatform;

typedef struct PwTitlePublicationDescriptor {
    PwTitleLaunch launch;
    uint8_t bytes[PW_TITLE_LAUNCH_MAX_BYTES];
    size_t length;
    char canonical_path[PW_TITLE_PUBLICATION_NATIVE_PATH];
} PwTitlePublicationDescriptor;

typedef struct PwTitlePublicationLease {
    int lock_fd;
    uint64_t lock_device;
    uint64_t lock_inode;
    PwGameProfile profile;
    PwGameInput preset;
    int has_preset;
} PwTitlePublicationLease;

void pw_title_publication_lease_init(PwTitlePublicationLease *lease);
void pw_title_publication_close(PwTitlePublicationLease *lease);

/* Parse the already bounded /app0/game-launch.json bytes before privilege is
 * granted. Records the canonical post-grant descriptor path. Output is left
 * unchanged on failure. */
int pw_title_publication_descriptor_parse(
    const uint8_t *bytes, size_t length, const char *expected_title_id,
    PwTitlePublicationDescriptor *descriptor, char *error, size_t error_size);

/* Post-grant checked read of the canonical descriptor. */
int pw_title_publication_read_descriptor(
    const char *canonical_path, const char *expected_title_id,
    const PwTitlePublicationPlatform *platform,
    PwTitlePublicationDescriptor *descriptor, char *error, size_t error_size);

/* Acquire and retain the publication reader lease, compare the canonical
 * descriptor with the pre-grant bytes, then admit the complete publication.
 * Returns 0, BUSY for lock contention, or INVALID for every other refusal. */
int pw_title_publication_open(
    const PwTitlePublicationDescriptor *before,
    const PwTitlePublicationPlatform *platform,
    PwTitlePublicationLease *lease, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
#endif
