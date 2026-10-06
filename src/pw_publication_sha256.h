/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PUBLICATION_SHA256_H
#define PW_PUBLICATION_SHA256_H
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/sha256.h"
#ifndef PW_PUBLICATION_MBEDTLS_CONFIG_H
#error "Compile provider and callers with the pinned pw_publication_config.h"
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum { PW_PUBLICATION_SHA256_BYTES = 32 };
typedef struct PwPublicationSha256 {
    mbedtls_sha256_context hash;
    uint64_t bytes;
    unsigned active;
} PwPublicationSha256;
/* init accepts an uninitialized stack object. Call init once before update,
 * finish, or clear. Re-init discards a previous digest; no allocation is used.
 * All functions return 0 on success, -1 on invalid input/provider failure.
 * NULL input is allowed only with zero length. Maximum length is UINT64_MAX/8
 * bytes (SHA-256's 64-bit bit count). Finish/errors clear initialized state;
 * a finished or failed context needs init before reuse. Output is cleared on
 * failure when non-NULL. Input/output must not overlap the context. */
int pw_publication_sha256_init(PwPublicationSha256 *context);
int pw_publication_sha256_update(PwPublicationSha256 *context, const void *bytes, size_t length);
int pw_publication_sha256_finish(PwPublicationSha256 *context, uint8_t output[PW_PUBLICATION_SHA256_BYTES]);
int pw_publication_sha256_bytes(const void *bytes, size_t length, uint8_t output[PW_PUBLICATION_SHA256_BYTES]);
void pw_publication_sha256_clear(PwPublicationSha256 *context);
#ifdef __cplusplus
}
#endif
#endif
