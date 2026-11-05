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
    /* A USB game (descriptor schema 3): no drive, or more than one, holds its
     * program. The publication itself passed everything checked so far. */
    PW_TITLE_PUBLICATION_ADMISSION_DRIVE_MISSING = -3,
    PW_TITLE_PUBLICATION_ADMISSION_DRIVE_AMBIGUOUS = -4,
    PW_TITLE_PUBLICATION_MOUNT = 16,
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
    /* A publisher that already knows where a USB game is (/mnt/usbN) names it,
     * and the game is checked there with no search: it can verify the game while
     * a copy on another drive would make the title refuse. NULL searches. */
    const char *known_game_mount;
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
    /* A USB game's drive, /mnt/usbN; empty for any other game. */
    char game_mount[PW_TITLE_PUBLICATION_MOUNT];
} PwTitlePublicationLease;

typedef struct PwTitlePublicationContents {
    PwGameProfile profile;
    PwGameInput preset;
    int has_preset;
    char game_mount[PW_TITLE_PUBLICATION_MOUNT];
} PwTitlePublicationContents;

/* Verify the exact frozen pending/committed receipt and metadata digest using
 * the same file semantics as reader admission. Caller must already hold the
 * qualified exclusive lease on borrowed_lock_fd through all subsequent effects.
 * This function checks its identity against the canonical stable lock; it does
 * not acquire, convert, release or close that descriptor and grants no startup
 * lease. Output is unchanged on failure. */
int pw_title_publication_verify_locked(
    const PwTitlePublicationDescriptor *descriptor,
    const PwTitlePublicationPlatform *platform, int borrowed_lock_fd,
    const uint8_t *expected_receipt_bytes, size_t expected_receipt_length,
    const char expected_metadata_sha256[65], PwTitlePublicationContents *contents,
    char *error, size_t error_size);

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
 * Returns 0, BUSY for lock contention, DRIVE_MISSING or DRIVE_AMBIGUOUS for a
 * USB game's drive (no lease is held, so the caller may ask again), or INVALID
 * for every other refusal. The admitted profile keeps its /mnt/usb paths, which
 * its digest covers; see pw_title_publication_use_game_mount. */
int pw_title_publication_open(
    const PwTitlePublicationDescriptor *before,
    const PwTitlePublicationPlatform *platform,
    PwTitlePublicationLease *lease, char *error, size_t error_size);

/* Put the drive found at admission (/mnt/usbN, or empty for none) into the
 * profile's paths: its executable and working folder, and any argument or
 * [environment] value that names the folder. Refused when a path no longer
 * fits; the profile may then be part changed, so the title must not start. */
int pw_title_publication_use_game_mount(PwGameProfile *profile, const char *mount,
                                        char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
#endif
