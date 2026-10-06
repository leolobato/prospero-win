/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_title_publication.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char digest_a[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char digest_b[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char digest_c[] =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

static void append(char *text, size_t capacity, const char *line)
{
    size_t used = strlen(text), length = strlen(line);
    assert(used + length < capacity);
    memcpy(text + used, line, length + 1u);
}

static void hex_encode(const char *plain, char *encoded, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    size_t length = strlen(plain);
    assert(length * 2u + 1u <= capacity);
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)plain[i];
        encoded[i * 2u] = digits[value >> 4u];
        encoded[i * 2u + 1u] = digits[value & 15u];
    }
    encoded[length * 2u] = '\0';
}

static void replace_once(char *text, size_t capacity,
                         const char *old, const char *replacement)
{
    char copy[40000];
    char *at = strstr(text, old);
    size_t prefix, suffix, replacement_length;
    assert(at);
    prefix = (size_t)(at - text);
    suffix = strlen(at + strlen(old));
    replacement_length = strlen(replacement);
    assert(prefix + replacement_length + suffix < capacity);
    memcpy(copy, text, prefix);
    memcpy(copy + prefix, replacement, replacement_length);
    memcpy(copy + prefix + replacement_length, at + strlen(old), suffix + 1u);
    memcpy(text, copy, prefix + replacement_length + suffix + 1u);
}

static const char committed_receipt[] =
    "{\"schema_version\":1,\"phase\":\"committed\","
    "\"transaction_id\":\"00000000-0000-1000-8000-000000000001\","
    "\"install_id\":\"install-220\",\"title_id\":\"FAKE78001\","
    "\"kit_id\":\"kit-hl2\",\"runtime_bundle_id\":\"wine-wow64\","
    "\"recipe_id\":\"steam-220-hl2-v1\",\"recipe_version\":\"1\","
    "\"previous_revision\":0,\"settings_revision\":18446744073709551615,"
    "\"plan_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"publication_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}";

static void valid_metadata(char *text, size_t capacity)
{
    char input_hex[129], dxvk_hex[1025], key0[2049], name0[129];
    char value0[129], key1[2049], name1[129], line[2300];
    hex_encode("gamepad.input", input_hex, sizeof(input_hex));
    hex_encode("/data/homebrew/FAKE78001/game/hl2/dxvk.conf", dxvk_hex, sizeof(dxvk_hex));
    hex_encode("Software\\Wine\\AppDefaults\\hl2.exe", key0, sizeof(key0));
    hex_encode("Version", name0, sizeof(name0));
    hex_encode("win7", value0, sizeof(value0));
    hex_encode("Control Panel\\Desktop", key1, sizeof(key1));
    hex_encode("LogPixels", name1, sizeof(name1));
    text[0] = '\0';
    append(text, capacity,
        "schema=2\ninstall_id=install-220\ntitle_id=FAKE78001\n"
        "data_root=/data/playden/games/install-220\n"
        "profile_id=playden-steam-220\nruntime_bundle_id=wine-wow64\n"
        "recipe_id=steam-220-hl2-v1\nrecipe_version=1\n"
        "settings_revision=18446744073709551615\n");
    snprintf(line, sizeof(line),
        "profile_sha256=%s\ndescriptor_sha256=%s\ncatalog_sha256=%s\n"
        "kit_id=kit-hl2\ninput_count=1\ninput.0.name_hex=%s\n"
        "input.0.sha256=%s\ndxvk.path_hex=%s\ndxvk.present=1\n"
        "dxvk.sha256=%s\nregistry_count=2\n",
        digest_a, digest_b, digest_c, input_hex, digest_a, dxvk_hex, digest_b);
    append(text, capacity, line);
    snprintf(line, sizeof(line),
        "registry.0.key_hex=%s\nregistry.0.name_hex=%s\n"
        "registry.0.kind=string\nregistry.0.string_hex=%s\nregistry.0.dword=\n"
        "registry.1.key_hex=%s\nregistry.1.name_hex=%s\n"
        "registry.1.kind=dword\nregistry.1.string_hex=\nregistry.1.dword=00000078\n",
        key0, name0, value0, key1, name1);
    append(text, capacity, line);
}

static void receipt_contract(void)
{
    PwTitlePublication parsed, kept;
    char text[10000];
    assert(pw_title_publication_parse((const uint8_t *)committed_receipt,
           strlen(committed_receipt), &parsed) == PW_OK);
    assert(parsed.phase == PW_TITLE_PUBLICATION_COMMITTED);
    assert(parsed.previous_revision == 0u && parsed.settings_revision == UINT64_MAX);
    assert(!strcmp(parsed.transaction_id, "00000000-0000-1000-8000-000000000001"));
    assert(!strcmp(parsed.publication_sha256, digest_b));

#define RECEIPT_REJECT(old, replacement) do { \
    strcpy(text, committed_receipt); replace_once(text, sizeof(text), old, replacement); \
    memset(&parsed, 0xa5, sizeof(parsed)); kept = parsed; \
    assert(pw_title_publication_parse((const uint8_t *)text, strlen(text), &parsed) != PW_OK); \
    assert(!memcmp(&parsed, &kept, sizeof(parsed))); \
} while (0)
    RECEIPT_REJECT("\"schema_version\":1", "\"schema_version\":2");
    RECEIPT_REJECT("\"phase\":\"committed\"", "\"phase\":\"ready\"");
    RECEIPT_REJECT("00000000-0000-1000-8000-000000000001", "00000000-0000-1000-8000-00000000000A");
    RECEIPT_REJECT("00000000-0000-1000-8000-000000000001", "00000000-0000-0000-0000-000000000000");
    RECEIPT_REJECT("\"previous_revision\":0,\"settings_revision\":18446744073709551615",
                   "\"previous_revision\":1,\"settings_revision\":0");
    RECEIPT_REJECT("\"settings_revision\":18446744073709551615", "\"settings_revision\":18446744073709551616");
    RECEIPT_REJECT(digest_a, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    RECEIPT_REJECT("\"schema_version\":1,", "\"schema_version\":1,\"schema_version\":1,");
    RECEIPT_REJECT("\"schema_version\":1,",
                   "\"schema_\\u0076ersion\":1,\"schema_version\":1,");
    RECEIPT_REJECT("\"schema_version\":1,", "\"unknown\":1,\"schema_version\":1,");
    RECEIPT_REJECT("\"kit_id\":\"kit-hl2\",", "");
    RECEIPT_REJECT("}", "} true");
#undef RECEIPT_REJECT

    strcpy(text, committed_receipt);
    replace_once(text, sizeof(text), "\"phase\":\"committed\"", "\"phase\":\"pending\"");
    replace_once(text, sizeof(text),
                 "\"publication_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"",
                 "\"publication_sha256\":\"\"");
    assert(pw_title_publication_parse((const uint8_t *)text, strlen(text), &parsed) == PW_OK);
    assert(parsed.phase == PW_TITLE_PUBLICATION_PENDING && !parsed.publication_sha256[0]);
    replace_once(text, sizeof(text), "\"publication_sha256\":\"\"",
                 "\"publication_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"");
    assert(pw_title_publication_parse((const uint8_t *)text, strlen(text), &parsed) != PW_OK);
    strcpy(text, committed_receipt);
    replace_once(text, sizeof(text),
                 "\"previous_revision\":0,\"settings_revision\":18446744073709551615",
                 "\"previous_revision\":18446744073709551615,\"settings_revision\":18446744073709551615");
    assert(pw_title_publication_parse((const uint8_t *)text, strlen(text), &parsed) == PW_OK);
    strcpy(text, committed_receipt);
    replace_once(text, sizeof(text),
                 "\"settings_revision\":18446744073709551615",
                 "\"settings_revision\":0");
    assert(pw_title_publication_parse((const uint8_t *)text, strlen(text), &parsed) == PW_OK);
    assert(pw_title_publication_parse((const uint8_t *)committed_receipt,
           PW_TITLE_PUBLICATION_MAX_BYTES + 1u, &parsed) == PW_ERR_LIMIT);
}

static void metadata_rejects(const char *old, const char *replacement)
{
    char text[40000];
    PwTitlePublicationMetadata parsed, kept;
    valid_metadata(text, sizeof(text));
    replace_once(text, sizeof(text), old, replacement);
    memset(&parsed, 0xa5, sizeof(parsed)); kept = parsed;
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) != PW_OK);
    assert(!memcmp(&parsed, &kept, sizeof(parsed)));
}

static void metadata_contract(void)
{
    char text[40000], input_hex[4097], duplicate[4600], plain[66];
    char bounded[1026], original_hex[4097];
    PwTitlePublicationMetadata parsed;
    valid_metadata(text, sizeof(text));
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_OK);
    assert(parsed.schema_version == 2u && parsed.settings_revision == UINT64_MAX);
    assert(!strcmp(parsed.profile_sha256, digest_a) &&
           !strcmp(parsed.descriptor_sha256, digest_b) &&
           !strcmp(parsed.catalog_sha256, digest_c));
    assert(parsed.input_count == 1u && !strcmp(parsed.inputs[0].name, "gamepad.input"));
    assert(parsed.dxvk_present == 1u && strstr(parsed.dxvk_path, "/hl2/dxvk.conf"));
    assert(parsed.registry_count == 2u);
    assert(parsed.registry[0].kind == PW_TITLE_PUBLICATION_REGISTRY_STRING);
    assert(!strcmp(parsed.registry[0].string_value, "win7"));
    assert(parsed.registry[1].kind == PW_TITLE_PUBLICATION_REGISTRY_DWORD &&
           parsed.registry[1].dword == 0x78u);

    metadata_rejects("schema=2", "schema=1");
    metadata_rejects("schema=2\n", "schema=2\nschema=2\n");
    metadata_rejects("schema=2\n", "unknown=x\nschema=2\n");
    metadata_rejects("settings_revision=18446744073709551615",
                     "settings_revision=18446744073709551616");
    metadata_rejects("input.0.name_hex=", "input.1.name_hex=");
    metadata_rejects("input_count=1", "input_count=65");
    metadata_rejects("registry_count=2", "registry_count=3");
    metadata_rejects("dxvk.present=1", "dxvk.present=2");
    metadata_rejects("dxvk.path_hex=2f", "dxvk.path_hex=2F");
    metadata_rejects("dxvk.sha256=bbbb", "dxvk.sha256=");
    metadata_rejects("registry.0.dword=", "registry.0.dword=00000000");
    metadata_rejects("registry.1.string_hex=", "registry.1.string_hex=00");
    metadata_rejects("registry.1.dword=00000078", "registry.1.dword=0000007A");
    metadata_rejects("\n", "\r\n");

    /* Paths admit 512 decoded bytes, registry keys 1024, and refuse one more. */
    valid_metadata(text, sizeof(text));
    hex_encode("/data/homebrew/FAKE78001/game/hl2/dxvk.conf",
               original_hex, sizeof(original_hex));
    bounded[0] = '/';
    memset(bounded + 1, 'a', 511u);
    bounded[512] = '\0';
    hex_encode(bounded, input_hex, sizeof(input_hex));
    replace_once(text, sizeof(text), original_hex, input_hex);
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_OK && strlen(parsed.dxvk_path) == 512u);
    bounded[512] = 'a';
    bounded[513] = '\0';
    hex_encode(bounded, input_hex, sizeof(input_hex));
    valid_metadata(text, sizeof(text));
    replace_once(text, sizeof(text), original_hex, input_hex);
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_ERR_LIMIT);

    valid_metadata(text, sizeof(text));
    hex_encode("Software\\Wine\\AppDefaults\\hl2.exe", original_hex,
               sizeof(original_hex));
    memset(bounded, 'k', 1024u);
    bounded[1024] = '\0';
    hex_encode(bounded, input_hex, sizeof(input_hex));
    replace_once(text, sizeof(text), original_hex, input_hex);
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_OK && strlen(parsed.registry[0].key) == 1024u);
    bounded[1024] = 'k';
    bounded[1025] = '\0';
    hex_encode(bounded, input_hex, sizeof(input_hex));
    valid_metadata(text, sizeof(text));
    replace_once(text, sizeof(text), original_hex, input_hex);
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_ERR_LIMIT);

    valid_metadata(text, sizeof(text));
    text[strlen(text) - 1u] = '\0';
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) != PW_OK);
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           PW_TITLE_PUBLICATION_METADATA_MAX_BYTES + 1u, &parsed) == PW_ERR_LIMIT);

    /* Absent DXVK still binds a canonical target path and has no digest. */
    valid_metadata(text, sizeof(text));
    replace_once(text, sizeof(text), "dxvk.present=1", "dxvk.present=0");
    replace_once(text, sizeof(text),
                 "dxvk.sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                 "dxvk.sha256=");
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_OK);
    assert(!parsed.dxvk_present && parsed.dxvk_path[0] && !parsed.dxvk_sha256[0]);

    /* Empty registry string is semantic data, not a missing field. */
    valid_metadata(text, sizeof(text));
    hex_encode("win7", input_hex, sizeof(input_hex));
    replace_once(text, sizeof(text), input_hex, "");
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_OK);
    assert(parsed.registry[0].kind == PW_TITLE_PUBLICATION_REGISTRY_STRING &&
           !parsed.registry[0].string_value[0]);

    memset(plain, 'x', 64u);
    plain[64] = '\0';
    hex_encode(plain, input_hex, sizeof(input_hex));
    valid_metadata(text, sizeof(text));
    replace_once(text, sizeof(text), "77696e37", input_hex);
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_OK && strlen(parsed.registry[0].string_value) == 64u);
    plain[64] = 'x';
    plain[65] = '\0';
    hex_encode(plain, input_hex, sizeof(input_hex));
    metadata_rejects("77696e37", input_hex);

    valid_metadata(text, sizeof(text));
    replace_once(text, sizeof(text), "registry.0.kind=string", "registry.0.kind=absent");
    replace_once(text, sizeof(text), "registry.0.string_hex=77696e37",
                 "registry.0.string_hex=");
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_OK &&
           parsed.registry[0].kind == PW_TITLE_PUBLICATION_REGISTRY_ABSENT);

    /* Sixty-four unique decoded names is the exact input bound. */
    valid_metadata(text, sizeof(text));
    replace_once(text, sizeof(text), "input_count=1", "input_count=64");
    for (unsigned index = 1; index < 64u; ++index) {
        char name[32];
        snprintf(name, sizeof(name), "preset-%02u.input", index);
        hex_encode(name, input_hex, sizeof(input_hex));
        snprintf(duplicate, sizeof(duplicate),
                 "input.%u.name_hex=%s\ninput.%u.sha256=%s\n",
                 index, input_hex, index, digest_c);
        append(text, sizeof(text), duplicate);
    }
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) == PW_OK && parsed.input_count == 64u);

    memset(input_hex, 'a', 65u);
    input_hex[65] = '\0';
    hex_encode(input_hex, duplicate, sizeof(duplicate));
    metadata_rejects("input.0.name_hex=67616d657061642e696e707574", duplicate);

    /* Case-insensitive registry targets and input filenames cannot alias. */
    valid_metadata(text, sizeof(text));
    replace_once(text, sizeof(text), "input_count=1", "input_count=2");
    hex_encode("GAMEPAD.input", input_hex, sizeof(input_hex));
    snprintf(duplicate, sizeof(duplicate),
             "input.1.name_hex=%s\ninput.1.sha256=%s\n", input_hex, digest_c);
    append(text, sizeof(text), duplicate);
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) != PW_OK);

    valid_metadata(text, sizeof(text));
    {
        char key1[2049], name0[129], name1[129];
        char old_line[2300], new_line[2300];
        hex_encode("Control Panel\\Desktop", key1, sizeof(key1));
        snprintf(old_line, sizeof(old_line), "registry.1.key_hex=%s", key1);
        hex_encode("SOFTWARE\\WINE\\APPDEFAULTS\\HL2.EXE", key1, sizeof(key1));
        snprintf(new_line, sizeof(new_line), "registry.1.key_hex=%s", key1);
        replace_once(text, sizeof(text), old_line, new_line);
        hex_encode("LogPixels", name1, sizeof(name1));
        snprintf(old_line, sizeof(old_line), "registry.1.name_hex=%s", name1);
        hex_encode("version", name0, sizeof(name0));
        snprintf(new_line, sizeof(new_line), "registry.1.name_hex=%s", name0);
        replace_once(text, sizeof(text), old_line, new_line);
    }
    assert(pw_title_publication_metadata_parse((const uint8_t *)text,
           strlen(text), &parsed) != PW_OK);
}

int main(void)
{
    receipt_contract();
    metadata_contract();
    puts("test_pw_title_publication: passed");
    return 0;
}
