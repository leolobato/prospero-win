/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_TITLE_PUBLICATION_H
#define PROSPERO_WIN_PW_TITLE_PUBLICATION_H

#include "../include/prospero_win.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PW_TITLE_PUBLICATION_MAX_BYTES = 8192,
    PW_TITLE_PUBLICATION_METADATA_MAX_BYTES = 32768,
    PW_TITLE_PUBLICATION_MAX_INPUTS = 64,
    PW_TITLE_PUBLICATION_MAX_REGISTRY = 2,
};

typedef enum PwTitlePublicationPhase {
    PW_TITLE_PUBLICATION_PENDING = 1,
    PW_TITLE_PUBLICATION_COMMITTED = 2,
} PwTitlePublicationPhase;

typedef struct PwTitlePublication {
    unsigned schema_version;
    PwTitlePublicationPhase phase;
    char transaction_id[37];
    char install_id[65];
    char title_id[10];
    char kit_id[65];
    char runtime_bundle_id[65];
    char recipe_id[65];
    char recipe_version[65];
    uint64_t previous_revision;
    uint64_t settings_revision;
    char plan_sha256[65];
    char publication_sha256[65];
} PwTitlePublication;

typedef struct PwTitlePublicationInput {
    char name[65];
    char sha256[65];
} PwTitlePublicationInput;

typedef enum PwTitlePublicationRegistryKind {
    PW_TITLE_PUBLICATION_REGISTRY_ABSENT = 1,
    PW_TITLE_PUBLICATION_REGISTRY_STRING = 2,
    PW_TITLE_PUBLICATION_REGISTRY_DWORD = 3,
} PwTitlePublicationRegistryKind;

typedef struct PwTitlePublicationRegistry {
    char key[1025];
    char name[65];
    PwTitlePublicationRegistryKind kind;
    char string_value[65];
    uint32_t dword;
} PwTitlePublicationRegistry;

typedef struct PwTitlePublicationMetadata {
    unsigned schema_version;
    char install_id[65];
    char title_id[10];
    char data_root[512];
    char profile_id[65];
    char runtime_bundle_id[65];
    char recipe_id[65];
    char recipe_version[65];
    uint64_t settings_revision;
    char profile_sha256[65];
    char descriptor_sha256[65];
    char catalog_sha256[65];
    char kit_id[65];
    unsigned input_count;
    PwTitlePublicationInput inputs[PW_TITLE_PUBLICATION_MAX_INPUTS];
    char dxvk_path[513];
    unsigned dxvk_present;
    char dxvk_sha256[65];
    unsigned registry_count;
    PwTitlePublicationRegistry registry[PW_TITLE_PUBLICATION_MAX_REGISTRY];
} PwTitlePublicationMetadata;

/* Both parsers are allocation-free, bounded, strict, and write output only on
 * success. Metadata validates structural/canonical semantics and duplicate
 * registry targets. The caller compares registry keys against the exact
 * executable-derived owned policy because that identity is not parser input. */
int pw_title_publication_parse(const uint8_t *bytes, size_t length,
                               PwTitlePublication *publication);
int pw_title_publication_metadata_parse(
    const uint8_t *bytes, size_t length,
    PwTitlePublicationMetadata *metadata);

#ifdef __cplusplus
}
#endif

#endif
