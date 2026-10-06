/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_publication_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void vector(const char *text, const char *expected) {
    uint8_t digest[32]; char hex[65];
    CHECK(pw_publication_sha256_bytes(text, strlen(text), digest) == 0);
    for (size_t i = 0; i < 32; ++i) snprintf(hex + 2*i, 3, "%02x", digest[i]);
    CHECK(!strcmp(hex, expected));
    for (size_t split = 0; split <= strlen(text); ++split) {
        PwPublicationSha256 stream;
        uint8_t streamed[32];
        CHECK(pw_publication_sha256_init(&stream) == 0);
        CHECK(pw_publication_sha256_update(&stream, text, split) == 0);
        CHECK(pw_publication_sha256_update(&stream, NULL, 0) == 0);
        CHECK(pw_publication_sha256_update(&stream, text + split, strlen(text) - split) == 0);
        CHECK(pw_publication_sha256_finish(&stream, streamed) == 0);
        CHECK(!memcmp(streamed, digest, 32));
        for (size_t i = 0; i < sizeof(stream); ++i) CHECK(((uint8_t *)&stream)[i] == 0);
    }
}
int main(void) {
    vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    vector("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    PwPublicationSha256 context; uint8_t output[32];
    char block[1000], hex[65];
    memset(block, 'a', sizeof(block));
    CHECK(pw_publication_sha256_init(&context) == 0);
    for (size_t i = 0; i < 1000; ++i)
        CHECK(pw_publication_sha256_update(&context, block, sizeof(block)) == 0);
    CHECK(pw_publication_sha256_finish(&context, output) == 0);
    for (size_t i = 0; i < sizeof(output); ++i) snprintf(hex + 2*i, 3, "%02x", output[i]);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    CHECK(pw_publication_sha256_init(NULL) == -1);
    CHECK(pw_publication_sha256_init(&context) == 0);
    CHECK(pw_publication_sha256_update(&context, NULL, 1) == -1);
    CHECK(context.active == 0 && context.bytes == 0);
    memset(output, 0xaa, sizeof(output));
    CHECK(pw_publication_sha256_finish(&context, output) == -1);
    for (size_t i = 0; i < sizeof(output); ++i) CHECK(output[i] == 0);
    CHECK(pw_publication_sha256_init(&context) == 0);
    context.bytes = UINT64_MAX / 8; /* Exercise rejection without reading huge input. */
    CHECK(pw_publication_sha256_update(&context, "x", 1) == -1);
    CHECK(context.active == 0 && context.bytes == 0);
    CHECK(pw_publication_sha256_init(&context) == 0);
    CHECK(pw_publication_sha256_finish(&context, NULL) == -1 && !context.active);
    CHECK(pw_publication_sha256_init(&context) == 0);
    pw_publication_sha256_clear(&context);
    for (size_t i = 0; i < sizeof(context); ++i) CHECK(((uint8_t *)&context)[i] == 0);
    CHECK(pw_publication_sha256_update(&context, "x", 1) == -1);
    CHECK(pw_publication_sha256_bytes(NULL, 1, output) == -1);
    CHECK(pw_publication_sha256_bytes(NULL, 0, output) == 0);
    CHECK(pw_publication_sha256_bytes("abc", 3, NULL) == -1);
    pw_publication_sha256_clear(NULL);
    puts("publication sha256 tests passed");
    return 0;
}
