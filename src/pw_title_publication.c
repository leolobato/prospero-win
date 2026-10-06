/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_title_publication.h"

#include <string.h>

typedef struct JsonReader {
    const uint8_t *next;
    const uint8_t *end;
} JsonReader;

static void json_space(JsonReader *reader)
{
    while (reader->next < reader->end &&
           (*reader->next == ' ' || *reader->next == '\t' ||
            *reader->next == '\r' || *reader->next == '\n'))
        ++reader->next;
}

static int json_take(JsonReader *reader, uint8_t wanted)
{
    json_space(reader);
    if (reader->next == reader->end || *reader->next != wanted)
        return 0;
    ++reader->next;
    return 1;
}

static int hex_nibble(uint8_t value, int lowercase_only)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (!lowercase_only && value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static int json_string(JsonReader *reader, char *output, size_t capacity)
{
    size_t length = 0;
    if (!json_take(reader, '"'))
        return PW_ERR_MALFORMED;
    while (reader->next < reader->end) {
        unsigned value = *reader->next++;
        if (value == '"') {
            output[length] = '\0';
            return PW_OK;
        }
        if (value == '\\') {
            if (reader->next == reader->end)
                return PW_ERR_TRUNCATED;
            value = *reader->next++;
            if (value == 'u') {
                value = 0;
                if ((size_t)(reader->end - reader->next) < 4u)
                    return PW_ERR_TRUNCATED;
                for (unsigned index = 0; index < 4u; ++index) {
                    int digit = hex_nibble(*reader->next++, 0);
                    if (digit < 0)
                        return PW_ERR_MALFORMED;
                    value = value * 16u + (unsigned)digit;
                }
            } else if (value != '"' && value != '\\' && value != '/') {
                return PW_ERR_MALFORMED;
            }
        }
        if (value < 0x20u || value > 0x7eu)
            return PW_ERR_MALFORMED;
        if (length + 1u >= capacity)
            return PW_ERR_LIMIT;
        output[length++] = (char)value;
    }
    return PW_ERR_TRUNCATED;
}

static int json_uint64(JsonReader *reader, uint64_t *output)
{
    uint64_t value = 0;
    json_space(reader);
    if (reader->next == reader->end || *reader->next < '0' ||
        *reader->next > '9')
        return PW_ERR_MALFORMED;
    if (*reader->next == '0' && reader->next + 1u < reader->end &&
        reader->next[1] >= '0' && reader->next[1] <= '9')
        return PW_ERR_MALFORMED;
    while (reader->next < reader->end && *reader->next >= '0' &&
           *reader->next <= '9') {
        unsigned digit = *reader->next++ - '0';
        if (value > (UINT64_MAX - digit) / 10u)
            return PW_ERR_OVERFLOW;
        value = value * 10u + digit;
    }
    *output = value;
    return PW_OK;
}

static int identifier(const char *text)
{
    if (!text[0] || !((text[0] >= 'a' && text[0] <= 'z') ||
                      (text[0] >= '0' && text[0] <= '9')))
        return 0;
    for (size_t index = 1u; text[index]; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_'))
            return 0;
    }
    return 1;
}

static int title_id(const char *text)
{
    if (strlen(text) != 9u)
        return 0;
    for (unsigned index = 0; index < 9u; ++index) {
        if (index < 4u ? text[index] < 'A' || text[index] > 'Z'
                       : text[index] < '0' || text[index] > '9')
            return 0;
    }
    return 1;
}

static int digest(const char *text, int allow_empty)
{
    size_t length = strlen(text);
    if (allow_empty && !length)
        return 1;
    if (length != 64u)
        return 0;
    for (size_t index = 0; index < length; ++index)
        if (hex_nibble((uint8_t)text[index], 1) < 0)
            return 0;
    return 1;
}

static int canonical_uuid(const char *text)
{
    int nonzero = 0;
    if (strlen(text) != 36u)
        return 0;
    for (unsigned index = 0; index < 36u; ++index) {
        if (index == 8u || index == 13u || index == 18u || index == 23u) {
            if (text[index] != '-')
                return 0;
        } else {
            int digit = hex_nibble((uint8_t)text[index], 1);
            if (digit < 0)
                return 0;
            nonzero |= digit;
        }
    }
    return nonzero != 0;
}

int pw_title_publication_parse(const uint8_t *bytes, size_t length,
                               PwTitlePublication *publication)
{
    static const char *const keys[] = {
        "schema_version", "phase", "transaction_id", "install_id", "title_id",
        "kit_id", "runtime_bundle_id", "recipe_id", "recipe_version",
        "previous_revision", "settings_revision", "plan_sha256",
        "publication_sha256",
    };
    PwTitlePublication candidate = {0};
    JsonReader reader;
    unsigned seen = 0;

    if (!bytes || !publication)
        return PW_ERR_PRECONDITION;
    if (!length || length > PW_TITLE_PUBLICATION_MAX_BYTES)
        return PW_ERR_LIMIT;
    reader.next = bytes;
    reader.end = bytes + length;
    if (!json_take(&reader, '{'))
        return PW_ERR_MALFORMED;
    for (;;) {
        char key[64];
        unsigned field;
        int status = json_string(&reader, key, sizeof(key));
        if (status != PW_OK)
            return status;
        for (field = 0; field < sizeof(keys) / sizeof(keys[0]); ++field)
            if (!strcmp(key, keys[field]))
                break;
        if (field == sizeof(keys) / sizeof(keys[0]) || (seen & (1u << field)))
            return PW_ERR_MALFORMED;
        if (!json_take(&reader, ':'))
            return PW_ERR_MALFORMED;
        if (field == 0u || field == 9u || field == 10u) {
            uint64_t value;
            status = json_uint64(&reader, &value);
            if (status != PW_OK)
                return status;
            if (field == 0u) {
                if (value != 1u)
                    return PW_ERR_UNSUPPORTED;
                candidate.schema_version = 1u;
            } else if (field == 9u) {
                candidate.previous_revision = value;
            } else {
                candidate.settings_revision = value;
            }
        } else {
            char *target = NULL;
            size_t capacity = 0;
#define JSON_TARGET(member) target = candidate.member; capacity = sizeof(candidate.member); break
            switch (field) {
                case 1u: {
                    char phase[16];
                    status = json_string(&reader, phase, sizeof(phase));
                    if (status != PW_OK)
                        return status;
                    if (!strcmp(phase, "pending"))
                        candidate.phase = PW_TITLE_PUBLICATION_PENDING;
                    else if (!strcmp(phase, "committed"))
                        candidate.phase = PW_TITLE_PUBLICATION_COMMITTED;
                    else
                        return PW_ERR_UNSUPPORTED;
                    break;
                }
                case 2u: JSON_TARGET(transaction_id);
                case 3u: JSON_TARGET(install_id);
                case 4u: JSON_TARGET(title_id);
                case 5u: JSON_TARGET(kit_id);
                case 6u: JSON_TARGET(runtime_bundle_id);
                case 7u: JSON_TARGET(recipe_id);
                case 8u: JSON_TARGET(recipe_version);
                case 11u: JSON_TARGET(plan_sha256);
                case 12u: JSON_TARGET(publication_sha256);
            }
#undef JSON_TARGET
            if (field != 1u) {
                status = json_string(&reader, target, capacity);
                if (status != PW_OK)
                    return status;
            }
        }
        seen |= 1u << field;
        json_space(&reader);
        if (json_take(&reader, '}'))
            break;
        if (!json_take(&reader, ','))
            return PW_ERR_MALFORMED;
    }
    json_space(&reader);
    if (reader.next != reader.end ||
        seen != (1u << (sizeof(keys) / sizeof(keys[0]))) - 1u)
        return PW_ERR_MALFORMED;
    if (!canonical_uuid(candidate.transaction_id) ||
        !identifier(candidate.install_id) || !title_id(candidate.title_id) ||
        !identifier(candidate.kit_id) || !identifier(candidate.runtime_bundle_id) ||
        !identifier(candidate.recipe_id) || !identifier(candidate.recipe_version) ||
        candidate.previous_revision > candidate.settings_revision ||
        !digest(candidate.plan_sha256, 0))
        return PW_ERR_MALFORMED;
    if ((candidate.phase == PW_TITLE_PUBLICATION_PENDING &&
         candidate.publication_sha256[0]) ||
        (candidate.phase == PW_TITLE_PUBLICATION_COMMITTED &&
         !digest(candidate.publication_sha256, 0)))
        return PW_ERR_MALFORMED;
    *publication = candidate;
    return PW_OK;
}

static int decimal(const uint8_t *text, size_t length, uint64_t *output)
{
    uint64_t value = 0;
    if (!length || (length > 1u && text[0] == '0'))
        return PW_ERR_MALFORMED;
    for (size_t index = 0; index < length; ++index) {
        unsigned digit;
        if (text[index] < '0' || text[index] > '9')
            return PW_ERR_MALFORMED;
        digit = text[index] - '0';
        if (value > (UINT64_MAX - digit) / 10u)
            return PW_ERR_OVERFLOW;
        value = value * 10u + digit;
    }
    *output = value;
    return PW_OK;
}

static int copy_plain(char *output, size_t capacity,
                      const uint8_t *text, size_t length, int allow_empty)
{
    if ((!allow_empty && !length) || length >= capacity)
        return length >= capacity ? PW_ERR_LIMIT : PW_ERR_MALFORMED;
    for (size_t index = 0; index < length; ++index)
        if (text[index] < 0x20u || text[index] > 0x7eu)
            return PW_ERR_MALFORMED;
    memcpy(output, text, length);
    output[length] = '\0';
    return PW_OK;
}

static int decode_hex(char *output, size_t capacity,
                      const uint8_t *text, size_t length, int allow_empty)
{
    size_t decoded;
    if (length & 1u)
        return PW_ERR_MALFORMED;
    decoded = length / 2u;
    if ((!allow_empty && !decoded) || decoded >= capacity)
        return decoded >= capacity ? PW_ERR_LIMIT : PW_ERR_MALFORMED;
    for (size_t index = 0; index < decoded; ++index) {
        int high = hex_nibble(text[index * 2u], 1);
        int low = hex_nibble(text[index * 2u + 1u], 1);
        unsigned value;
        if (high < 0 || low < 0)
            return PW_ERR_MALFORMED;
        value = (unsigned)((high << 4) | low);
        if (value < 0x20u || value > 0x7eu)
            return PW_ERR_MALFORMED;
        output[index] = (char)value;
    }
    output[decoded] = '\0';
    return PW_OK;
}

static int copy_digest(char output[65], const uint8_t *text,
                       size_t length, int allow_empty)
{
    if (allow_empty && !length) {
        output[0] = '\0';
        return PW_OK;
    }
    if (length != 64u)
        return PW_ERR_MALFORMED;
    for (size_t index = 0; index < length; ++index)
        if (hex_nibble(text[index], 1) < 0)
            return PW_ERR_MALFORMED;
    memcpy(output, text, length);
    output[length] = '\0';
    return PW_OK;
}

static int canonical_path(const char *path)
{
    size_t length = strlen(path), component = 1u;
    if (length < 2u || path[0] != '/' || path[length - 1u] == '/')
        return 0;
    for (size_t index = 1u; index <= length; ++index) {
        unsigned char value = (unsigned char)path[index];
        if (value == '\\' || (value && (value < 0x20u || value == 0x7fu)))
            return 0;
        if (value == '/' || value == '\0') {
            size_t part = index - component;
            if (!part || (part == 1u && path[component] == '.') ||
                (part == 2u && path[component] == '.' && path[component + 1u] == '.'))
                return 0;
            component = index + 1u;
        }
    }
    return 1;
}

static int input_name(const char *name)
{
    size_t length = strlen(name);
    if (length <= 6u || strcmp(name + length - 6u, ".input"))
        return 0;
    for (size_t index = 0; index < length; ++index)
        if (name[index] == '/' || name[index] == '\\' || name[index] == ':')
            return 0;
    return strcmp(name, ".input") != 0 && strcmp(name, ".") != 0 &&
           strcmp(name, "..") != 0;
}

static unsigned char ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
        ? (unsigned char)(value + ('a' - 'A')) : value;
}

static int equal_case(const char *left, const char *right)
{
    while (*left && *right) {
        if (ascii_lower((unsigned char)*left++) !=
            ascii_lower((unsigned char)*right++))
            return 0;
    }
    return *left == *right;
}

static int indexed_key(const char *key, const char *prefix,
                       unsigned maximum, unsigned *index, const char **suffix)
{
    size_t prefix_length = strlen(prefix);
    const char *next;
    unsigned value = 0;
    if (strncmp(key, prefix, prefix_length))
        return 0;
    next = key + prefix_length;
    if (*next < '0' || *next > '9' || (*next == '0' && next[1] != '.'))
        return -1;
    while (*next >= '0' && *next <= '9') {
        value = value * 10u + (unsigned)(*next++ - '0');
        if (value >= maximum)
            return -1;
    }
    if (*next != '.')
        return -1;
    *index = value;
    *suffix = next + 1;
    return 1;
}

int pw_title_publication_metadata_parse(
    const uint8_t *bytes, size_t length,
    PwTitlePublicationMetadata *metadata)
{
    enum {
        BASE_SCHEMA, BASE_INSTALL, BASE_TITLE, BASE_DATA, BASE_PROFILE,
        BASE_RUNTIME, BASE_RECIPE, BASE_RECIPE_VERSION, BASE_REVISION,
        BASE_PROFILE_HASH, BASE_DESCRIPTOR_HASH, BASE_CATALOG_HASH, BASE_KIT,
        BASE_INPUT_COUNT, BASE_DXVK_PATH, BASE_DXVK_PRESENT, BASE_DXVK_HASH,
        BASE_REGISTRY_COUNT, BASE_COUNT
    };
    static const char *const base_keys[BASE_COUNT] = {
        "schema", "install_id", "title_id", "data_root", "profile_id",
        "runtime_bundle_id", "recipe_id", "recipe_version", "settings_revision",
        "profile_sha256", "descriptor_sha256", "catalog_sha256", "kit_id",
        "input_count", "dxvk.path_hex", "dxvk.present", "dxvk.sha256",
        "registry_count",
    };
    PwTitlePublicationMetadata candidate = {0};
    uint8_t input_seen[PW_TITLE_PUBLICATION_MAX_INPUTS] = {0};
    uint8_t registry_seen[PW_TITLE_PUBLICATION_MAX_REGISTRY] = {0};
    uint8_t registry_dword_value[PW_TITLE_PUBLICATION_MAX_REGISTRY] = {0};
    uint32_t base_seen = 0;
    size_t offset = 0;

    if (!bytes || !metadata)
        return PW_ERR_PRECONDITION;
    if (!length || length > PW_TITLE_PUBLICATION_METADATA_MAX_BYTES)
        return PW_ERR_LIMIT;
    if (bytes[length - 1u] != '\n' || memchr(bytes, '\r', length) ||
        memchr(bytes, '\0', length))
        return PW_ERR_MALFORMED;
    while (offset < length) {
        const uint8_t *line = bytes + offset;
        const uint8_t *newline = memchr(line, '\n', length - offset);
        const uint8_t *equals;
        char key[64];
        size_t line_length, key_length, value_length;
        const uint8_t *value;
        int handled = 0, status = PW_OK;
        if (!newline)
            return PW_ERR_TRUNCATED;
        line_length = (size_t)(newline - line);
        if (!line_length)
            return PW_ERR_MALFORMED;
        equals = memchr(line, '=', line_length);
        if (!equals)
            return PW_ERR_MALFORMED;
        key_length = (size_t)(equals - line);
        if (!key_length || key_length >= sizeof(key))
            return PW_ERR_MALFORMED;
        memcpy(key, line, key_length);
        key[key_length] = '\0';
        value = equals + 1u;
        value_length = (size_t)(newline - value);
        for (unsigned field = 0; field < BASE_COUNT; ++field) {
            if (strcmp(key, base_keys[field]))
                continue;
            if (base_seen & (1u << field))
                return PW_ERR_MALFORMED;
            base_seen |= 1u << field;
            handled = 1;
            if (field == BASE_SCHEMA || field == BASE_REVISION ||
                field == BASE_INPUT_COUNT || field == BASE_DXVK_PRESENT ||
                field == BASE_REGISTRY_COUNT) {
                uint64_t number;
                status = decimal(value, value_length, &number);
                if (status != PW_OK)
                    return status;
                if (field == BASE_SCHEMA) {
                    if (number != 2u)
                        return PW_ERR_UNSUPPORTED;
                    candidate.schema_version = 2u;
                } else if (field == BASE_REVISION) {
                    candidate.settings_revision = number;
                } else if (field == BASE_INPUT_COUNT) {
                    if (number > PW_TITLE_PUBLICATION_MAX_INPUTS)
                        return PW_ERR_LIMIT;
                    candidate.input_count = (unsigned)number;
                } else if (field == BASE_DXVK_PRESENT) {
                    if (number > 1u)
                        return PW_ERR_MALFORMED;
                    candidate.dxvk_present = (unsigned)number;
                } else {
                    if (number > PW_TITLE_PUBLICATION_MAX_REGISTRY)
                        return PW_ERR_LIMIT;
                    candidate.registry_count = (unsigned)number;
                }
            } else if (field == BASE_PROFILE_HASH || field == BASE_DESCRIPTOR_HASH ||
                       field == BASE_CATALOG_HASH || field == BASE_DXVK_HASH) {
                char *target = field == BASE_PROFILE_HASH ? candidate.profile_sha256 :
                    field == BASE_DESCRIPTOR_HASH ? candidate.descriptor_sha256 :
                    field == BASE_CATALOG_HASH ? candidate.catalog_sha256 : candidate.dxvk_sha256;
                status = copy_digest(target, value, value_length, field == BASE_DXVK_HASH);
            } else if (field == BASE_DXVK_PATH) {
                status = decode_hex(candidate.dxvk_path, sizeof(candidate.dxvk_path),
                                    value, value_length, 0);
            } else {
                char *target = field == BASE_INSTALL ? candidate.install_id :
                    field == BASE_TITLE ? candidate.title_id :
                    field == BASE_DATA ? candidate.data_root :
                    field == BASE_PROFILE ? candidate.profile_id :
                    field == BASE_RUNTIME ? candidate.runtime_bundle_id :
                    field == BASE_RECIPE ? candidate.recipe_id :
                    field == BASE_RECIPE_VERSION ? candidate.recipe_version : candidate.kit_id;
                size_t capacity = field == BASE_TITLE ? sizeof(candidate.title_id) :
                    field == BASE_DATA ? sizeof(candidate.data_root) : sizeof(candidate.install_id);
                status = copy_plain(target, capacity, value, value_length, 0);
            }
            break;
        }
        if (!handled) {
            unsigned index;
            const char *suffix;
            int indexed = indexed_key(key, "input.", PW_TITLE_PUBLICATION_MAX_INPUTS,
                                      &index, &suffix);
            if (indexed < 0)
                return PW_ERR_LIMIT;
            if (indexed > 0) {
                unsigned bit;
                if (!strcmp(suffix, "name_hex")) {
                    bit = 1u;
                    status = decode_hex(candidate.inputs[index].name,
                                        sizeof(candidate.inputs[index].name),
                                        value, value_length, 0);
                } else if (!strcmp(suffix, "sha256")) {
                    bit = 2u;
                    status = copy_digest(candidate.inputs[index].sha256,
                                         value, value_length, 0);
                } else {
                    return PW_ERR_MALFORMED;
                }
                if (input_seen[index] & bit)
                    return PW_ERR_MALFORMED;
                input_seen[index] |= (uint8_t)bit;
                handled = 1;
            }
        }
        if (!handled) {
            unsigned index, bit;
            const char *suffix;
            int indexed = indexed_key(key, "registry.", PW_TITLE_PUBLICATION_MAX_REGISTRY,
                                      &index, &suffix);
            if (indexed < 0)
                return PW_ERR_LIMIT;
            if (!indexed)
                return PW_ERR_MALFORMED;
            if (!strcmp(suffix, "key_hex")) {
                bit = 1u;
                status = decode_hex(candidate.registry[index].key,
                                    sizeof(candidate.registry[index].key),
                                    value, value_length, 0);
            } else if (!strcmp(suffix, "name_hex")) {
                bit = 2u;
                status = decode_hex(candidate.registry[index].name,
                                    sizeof(candidate.registry[index].name),
                                    value, value_length, 0);
            } else if (!strcmp(suffix, "kind")) {
                char kind[16];
                bit = 4u;
                status = copy_plain(kind, sizeof(kind), value, value_length, 0);
                if (status == PW_OK) {
                    if (!strcmp(kind, "absent"))
                        candidate.registry[index].kind = PW_TITLE_PUBLICATION_REGISTRY_ABSENT;
                    else if (!strcmp(kind, "string"))
                        candidate.registry[index].kind = PW_TITLE_PUBLICATION_REGISTRY_STRING;
                    else if (!strcmp(kind, "dword"))
                        candidate.registry[index].kind = PW_TITLE_PUBLICATION_REGISTRY_DWORD;
                    else
                        status = PW_ERR_UNSUPPORTED;
                }
            } else if (!strcmp(suffix, "string_hex")) {
                bit = 8u;
                status = decode_hex(candidate.registry[index].string_value,
                                    sizeof(candidate.registry[index].string_value),
                                    value, value_length, 1);
            } else if (!strcmp(suffix, "dword")) {
                bit = 16u;
                if (!value_length) {
                    candidate.registry[index].dword = 0;
                } else if (value_length == 8u) {
                    uint32_t number = 0;
                    registry_dword_value[index] = 1u;
                    for (size_t digit = 0; digit < 8u; ++digit) {
                        int nibble = hex_nibble(value[digit], 1);
                        if (nibble < 0)
                            return PW_ERR_MALFORMED;
                        number = number * 16u + (unsigned)nibble;
                    }
                    candidate.registry[index].dword = number;
                } else {
                    return PW_ERR_MALFORMED;
                }
            } else {
                return PW_ERR_MALFORMED;
            }
            if (registry_seen[index] & bit)
                return PW_ERR_MALFORMED;
            registry_seen[index] |= (uint8_t)bit;
        }
        if (status != PW_OK)
            return status;
        offset += line_length + 1u;
    }
    if (base_seen != (1u << BASE_COUNT) - 1u)
        return PW_ERR_MALFORMED;
    for (unsigned index = 0; index < PW_TITLE_PUBLICATION_MAX_INPUTS; ++index) {
        if (index < candidate.input_count) {
            if (input_seen[index] != 3u || !input_name(candidate.inputs[index].name))
                return PW_ERR_MALFORMED;
            for (unsigned prior = 0; prior < index; ++prior)
                if (equal_case(candidate.inputs[prior].name, candidate.inputs[index].name))
                    return PW_ERR_MALFORMED;
        } else if (input_seen[index]) {
            return PW_ERR_MALFORMED;
        }
    }
    for (unsigned index = 0; index < PW_TITLE_PUBLICATION_MAX_REGISTRY; ++index) {
        PwTitlePublicationRegistry *entry = &candidate.registry[index];
        if (index < candidate.registry_count) {
            if (registry_seen[index] != 31u)
                return PW_ERR_MALFORMED;
            if (entry->kind == PW_TITLE_PUBLICATION_REGISTRY_DWORD) {
                if (entry->string_value[0] || !registry_dword_value[index])
                    return PW_ERR_MALFORMED;
            } else if (entry->kind == PW_TITLE_PUBLICATION_REGISTRY_STRING) {
                if (registry_dword_value[index])
                    return PW_ERR_MALFORMED;
            } else if (entry->kind == PW_TITLE_PUBLICATION_REGISTRY_ABSENT) {
                if (entry->string_value[0] || registry_dword_value[index])
                    return PW_ERR_MALFORMED;
            } else {
                return PW_ERR_MALFORMED;
            }
            for (unsigned prior = 0; prior < index; ++prior)
                if (equal_case(candidate.registry[prior].key, entry->key) &&
                    equal_case(candidate.registry[prior].name, entry->name))
                    return PW_ERR_MALFORMED;
        } else if (registry_seen[index]) {
            return PW_ERR_MALFORMED;
        }
    }
    if (!identifier(candidate.install_id) || !title_id(candidate.title_id) ||
        !identifier(candidate.profile_id) || !identifier(candidate.runtime_bundle_id) ||
        !identifier(candidate.recipe_id) || !identifier(candidate.recipe_version) ||
        !identifier(candidate.kit_id) || !canonical_path(candidate.data_root) ||
        !canonical_path(candidate.dxvk_path))
        return PW_ERR_MALFORMED;
    {
        static const char root[] = "/data/playden/games/";
        if (strncmp(candidate.data_root, root, sizeof(root) - 1u) ||
            strcmp(candidate.data_root + sizeof(root) - 1u, candidate.install_id))
            return PW_ERR_MALFORMED;
    }
    if ((candidate.dxvk_present && !digest(candidate.dxvk_sha256, 0)) ||
        (!candidate.dxvk_present && candidate.dxvk_sha256[0]))
        return PW_ERR_MALFORMED;
    *metadata = candidate;
    return PW_OK;
}
