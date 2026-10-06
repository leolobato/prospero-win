/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_publication_sha256.h"
#include "mbedtls/platform_util.h"
#include <string.h>

/* Configuration hook; keep vendor sources byte-identical to their archive.
 * Volatile stores preserve clearing under optimization without libc allocators. */
void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;
    while (length--) *bytes++ = 0;
}

void pw_publication_sha256_clear(PwPublicationSha256 *context)
{
    if (context) mbedtls_platform_zeroize(context, sizeof(*context));
}

int pw_publication_sha256_init(PwPublicationSha256 *context)
{
    if (!context) return -1;
    /* Never inspect an uninitialized caller's stack bytes. */
    pw_publication_sha256_clear(context);
    mbedtls_sha256_init(&context->hash);
    if (mbedtls_sha256_starts(&context->hash, 0) != 0) {
        pw_publication_sha256_clear(context);
        return -1;
    }
    context->active = 1;
    return 0;
}

int pw_publication_sha256_update(PwPublicationSha256 *context, const void *bytes, size_t length)
{
    if (!context) return -1;
    if (!context->active || (!bytes && length) ||
        context->bytes > UINT64_MAX / 8 || length > UINT64_MAX / 8 - context->bytes) {
        pw_publication_sha256_clear(context);
        return -1;
    }
    if (length && mbedtls_sha256_update(&context->hash, bytes, length) != 0) {
        pw_publication_sha256_clear(context);
        return -1;
    }
    context->bytes += length;
    return 0;
}

int pw_publication_sha256_finish(PwPublicationSha256 *context, uint8_t output[32])
{
    uint8_t digest[32];
    if (!context || !context->active || !output) {
        if (output) memset(output, 0, 32);
        pw_publication_sha256_clear(context);
        return -1;
    }
    if (mbedtls_sha256_finish(&context->hash, digest) != 0) {
        mbedtls_platform_zeroize(digest, sizeof(digest));
        memset(output, 0, 32);
        pw_publication_sha256_clear(context);
        return -1;
    }
    memcpy(output, digest, 32);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    pw_publication_sha256_clear(context);
    return 0;
}

int pw_publication_sha256_bytes(const void *bytes, size_t length, uint8_t output[32])
{
    PwPublicationSha256 context;
    if (!output) return -1;
    if (pw_publication_sha256_init(&context) != 0 ||
        pw_publication_sha256_update(&context, bytes, length) != 0) {
        memset(output, 0, 32);
        pw_publication_sha256_clear(&context);
        return -1;
    }
    return pw_publication_sha256_finish(&context, output);
}
