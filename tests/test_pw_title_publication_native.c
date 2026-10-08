/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_title_publication_native.h"
#include "../src/pw_title_publication.h"
#include "../src/pw_publication_sha256.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct Fixture {
    char root[256];
    PwTitlePublicationPlatform platform;
    PwTitlePublicationDescriptor before;
    char game_root[1024];
    char descriptor[8192];
    char profile[8192];
    char input[8192];
    char metadata[32768];
    char receipt[8192];
} Fixture;

static void path(char *out, size_t size, const Fixture *f, const char *canonical)
{
    CHECK(canonical[0] == '/');
    CHECK(snprintf(out, size, "%s%s", f->root, canonical) > 0);
}

static void directory(Fixture *f, const char *canonical, mode_t mode)
{
    char mapped[1200], copy[1200];
    path(mapped, sizeof(mapped), f, canonical);
    strcpy(copy, mapped);
    for (char *p = copy + strlen(f->root) + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(copy, 0755) && errno != EEXIST) CHECK(0);
        *p = '/';
    }
    if (mkdir(copy, mode) && errno != EEXIST) CHECK(0);
    CHECK(chmod(copy, mode) == 0);
}

static void write_file(Fixture *f, const char *canonical, const void *bytes,
                       size_t length, mode_t mode)
{
    char mapped[1200];
    path(mapped, sizeof(mapped), f, canonical);
    int fd = open(mapped, O_WRONLY | O_CREAT | O_TRUNC, mode);
    CHECK(fd >= 0 && fchmod(fd, mode) == 0);
    CHECK(write(fd, bytes, length) == (ssize_t)length);
    CHECK(close(fd) == 0);
}

static void read_file(Fixture *f, const char *canonical, char *bytes, size_t capacity)
{
    char mapped[1200];
    path(mapped, sizeof(mapped), f, canonical);
    int fd = open(mapped, O_RDONLY);
    CHECK(fd >= 0);
    ssize_t count = read(fd, bytes, capacity - 1);
    CHECK(count >= 0 && close(fd) == 0);
    bytes[count] = 0;
}

static void digest(const void *bytes, size_t length, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t raw[32];
    CHECK(pw_publication_sha256_bytes(bytes, length, raw) == 0);
    for (size_t i = 0; i < sizeof(raw); ++i) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[64] = 0;
}

static void hex(const char *plain, char *out, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    size_t length = strlen(plain);
    CHECK(length * 2 + 1 <= capacity);
    for (size_t i = 0; i < length; ++i) {
        unsigned value = (unsigned char)plain[i];
        out[i * 2] = digits[value >> 4]; out[i * 2 + 1] = digits[value & 15];
    }
    out[length * 2] = 0;
}

static void replace_once(char *text, size_t capacity, const char *old, const char *replacement)
{
    char *at = strstr(text, old); size_t prefix, suffix, replacement_length;
    CHECK(at); prefix = (size_t)(at - text); suffix = strlen(at + strlen(old));
    replacement_length = strlen(replacement); CHECK(prefix + replacement_length + suffix < capacity);
    memmove(at + replacement_length, at + strlen(old), suffix + 1);
    memcpy(at, replacement, replacement_length);
}

static int host_lock(void *context, int fd)
{
    int *calls = context;
    ++*calls;
    return flock(fd, LOCK_SH | LOCK_NB);
}

static int unavailable_lock(void *context, int fd)
{
    (void)context; (void)fd; errno = ENOSYS; return -1;
}

static void fixture_refresh(Fixture *f)
{
    char profile_hash[65], descriptor_hash[65], catalog_hash[65], input_hash[65];
    char dxvk_hash[65], metadata_hash[65];
    char input_name[129], dxvk_path[1025], key0[2049], name0[129], value0[129];
    char key1[2049], name1[129];
    const char catalog[] = "playden-steam-220.profile\n";
    const char dxvk[] = "dxgi.maxFrameLatency = 1\n";
    const char user_reg[] =
        "WINE REGISTRY Version 2\n\n"
        "[Control Panel\\\\Desktop]\n\"LogPixels\"=dword:00000078\n\n"
        "[Software\\\\Wine\\\\AppDefaults\\\\hl2.exe] 123\n"
        "\"Version\"=\"win7\"\n\n"
        "[Software\\\\Wine\\\\Unrelated]\n\"Keep\"=\"yes\"\n";

    digest(f->profile, strlen(f->profile), profile_hash);
    digest(f->descriptor, strlen(f->descriptor), descriptor_hash);
    digest(catalog, strlen(catalog), catalog_hash);
    digest(f->input, strlen(f->input), input_hash);
    digest(dxvk, strlen(dxvk), dxvk_hash);
    hex("gamepad.input", input_name, sizeof(input_name));
    char dxvk_file[1100];
    CHECK(snprintf(dxvk_file, sizeof(dxvk_file), "%s/dxvk.conf", f->game_root) > 0);
    hex(dxvk_file, dxvk_path, sizeof(dxvk_path));
    hex("Software\\Wine\\AppDefaults\\hl2.exe", key0, sizeof(key0));
    hex("Version", name0, sizeof(name0)); hex("win7", value0, sizeof(value0));
    hex("Control Panel\\Desktop", key1, sizeof(key1)); hex("LogPixels", name1, sizeof(name1));
    CHECK(snprintf(f->metadata, sizeof(f->metadata),
        "schema=2\ninstall_id=install-220\ntitle_id=FAKE78001\n"
        "data_root=/data/playden/games/install-220\nprofile_id=playden-steam-220\n"
        "runtime_bundle_id=wine-wow64\nrecipe_id=steam-220-hl2-v1\nrecipe_version=1\n"
        "settings_revision=7\nprofile_sha256=%s\ndescriptor_sha256=%s\n"
        "catalog_sha256=%s\nkit_id=kit-hl2\ninput_count=1\n"
        "input.0.name_hex=%s\ninput.0.sha256=%s\ndxvk.path_hex=%s\n"
        "dxvk.present=1\ndxvk.sha256=%s\nregistry_count=2\n"
        "registry.0.key_hex=%s\nregistry.0.name_hex=%s\nregistry.0.kind=string\n"
        "registry.0.string_hex=%s\nregistry.0.dword=\n"
        "registry.1.key_hex=%s\nregistry.1.name_hex=%s\nregistry.1.kind=dword\n"
        "registry.1.string_hex=\nregistry.1.dword=00000078\n",
        profile_hash, descriptor_hash, catalog_hash, input_name, input_hash,
        dxvk_path, dxvk_hash, key0, name0, value0, key1, name1) > 0);
    digest(f->metadata, strlen(f->metadata), metadata_hash);
    CHECK(snprintf(f->receipt, sizeof(f->receipt),
        "{\"schema_version\":1,\"phase\":\"committed\","
        "\"transaction_id\":\"00000000-0000-1000-8000-000000000001\","
        "\"install_id\":\"install-220\",\"title_id\":\"FAKE78001\","
        "\"kit_id\":\"kit-hl2\",\"runtime_bundle_id\":\"wine-wow64\","
        "\"recipe_id\":\"steam-220-hl2-v1\",\"recipe_version\":\"1\","
        "\"previous_revision\":6,\"settings_revision\":7,"
        "\"plan_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"publication_sha256\":\"%s\"}\n", metadata_hash) > 0);
    write_file(f, "/data/homebrew/FAKE78001/game-launch.json", f->descriptor, strlen(f->descriptor), 0600);
    write_file(f, "/data/playden/games/install-220/library/profiles/playden-steam-220.profile", f->profile, strlen(f->profile), 0600);
    write_file(f, "/data/playden/games/install-220/library/profiles/profiles.lst", catalog, strlen(catalog), 0600);
    write_file(f, "/data/playden/games/install-220/library/input/gamepad.input", f->input, strlen(f->input), 0600);
    write_file(f, "/data/playden/games/install-220/library/profiles/.playden-publication", f->metadata, strlen(f->metadata), 0600);
    write_file(f, "/data/playden/games/install-220/.playden-control/publication-state.v1", f->receipt, strlen(f->receipt), 0600);
    write_file(f, dxvk_file, dxvk, strlen(dxvk), 0644);
    write_file(f, "/data/playden/games/install-220/library/prefixes/game/user.reg", user_reg, strlen(user_reg), 0644);
}

static void publish_modified_metadata(Fixture *f)
{
    char old_hash[65], new_hash[65];
    read_file(f, "/data/playden/games/install-220/library/profiles/.playden-publication",
              f->metadata, sizeof(f->metadata));
    digest(f->metadata, strlen(f->metadata), new_hash);
    read_file(f, "/data/playden/games/install-220/.playden-control/publication-state.v1",
              f->receipt, sizeof(f->receipt));
    const char *field = strstr(f->receipt, "\"publication_sha256\":\""); CHECK(field);
    field += strlen("\"publication_sha256\":\""); CHECK(strlen(field) >= 64);
    memcpy(old_hash, field, 64); old_hash[64] = 0;
    replace_once(f->receipt, sizeof(f->receipt), old_hash, new_hash);
    write_file(f, "/data/playden/games/install-220/.playden-control/publication-state.v1",
               f->receipt, strlen(f->receipt), 0600);
}

static void fixture_init(Fixture *f, int *lock_calls)
{
    memset(f, 0, sizeof(*f));
    /* A canonical temporary root: macOS /tmp is a symlink that admission refuses. */
    strcpy(f->root, access("/private/tmp", W_OK) == 0 ? "/private/tmp/pw-publication-native.XXXXXX"
                                                    : "/tmp/pw-publication-native.XXXXXX");
    CHECK(mkdtemp(f->root));
    f->platform.host_root = f->root; f->platform.owner_uid = (uint32_t)geteuid();
    f->platform.try_lock_shared = host_lock; f->platform.context = lock_calls;
    const char *private_directories[] = {
        "/data/playden/games/install-220", "/data/playden/games/install-220/library",
        "/data/playden/games/install-220/library/profiles", "/data/playden/games/install-220/library/input",
        "/data/playden/games/install-220/library/prefixes", "/data/playden/games/install-220/library/prefixes/game",
        "/data/playden/games/install-220/.playden-control", NULL};
    for (const char **name = private_directories; *name; ++name) directory(f, *name, 0700);
    directory(f, "/data/homebrew/FAKE78001", 0755);
    directory(f, "/data/homebrew/FAKE78001/games/220", 0755);
    write_file(f, "/data/playden/games/install-220/.playden-control/publication.lock", "", 0, 0600);
    strcpy(f->game_root, "/data/homebrew/FAKE78001/games/220");
    strcpy(f->descriptor,
        "{\"schema_version\":1,\"app_id\":220,\"settings_revision\":7,"
        "\"title_id\":\"FAKE78001\",\"install_id\":\"install-220\","
        "\"data_root\":\"/data/playden/games/install-220\","
        "\"default_profile_id\":\"playden-steam-220\","
        "\"runtime_bundle_id\":\"wine-wow64\",\"recipe_id\":\"steam-220-hl2-v1\","
        "\"recipe_version\":\"1\",\"exit_policy\":\"exit-title\"}\n");
    strcpy(f->profile,
        "[application]\nid = playden-steam-220\nname = Half-Life 2\n"
        "executable = Z:\\data\\homebrew\\FAKE78001\\games\\220\\hl2.exe\n"
        "working_directory = Z:\\data\\homebrew\\FAKE78001\\games\\220\n"
        "arguments = -steam\nprefix = game\nruntime = wine-wow64\narchitecture = pe64\n"
        "graphics = dxvk\n\n[input]\npreset = gamepad\nmode = keyboard\ncross = space\n");
    strcpy(f->input, "[input]\nmode = keyboard\ncross = space\n");
    fixture_refresh(f);
    CHECK(pw_title_publication_descriptor_parse((const uint8_t *)f->descriptor,
          strlen(f->descriptor), "FAKE78001", &f->before, NULL, 0) == PW_OK);
}

static void fixture_destroy(Fixture *f)
{
    char command[400]; CHECK(snprintf(command, sizeof(command), "rm -rf '%s'", f->root) > 0);
    CHECK(system(command) == 0);
}

static int admit(Fixture *f, PwTitlePublicationLease *lease)
{
    char error[256];
    pw_title_publication_lease_init(lease);
    error[0] = 0;
    int result = pw_title_publication_open(&f->before, &f->platform, lease, error, sizeof(error));
    if (getenv("PW_TEST_TRACE") && result) fprintf(stderr, "admit: %d %s\n", result, error);
    return result;
}

static void valid_and_descriptor_contract(void)
{
    Fixture f; int calls = 0; fixture_init(&f, &calls);
    PwTitlePublicationLease lease; memset(&lease, 0xa5, sizeof(lease));
    CHECK(admit(&f, &lease) == 0 && lease.lock_fd >= 0 && calls == 1);
    CHECK(!strcmp(lease.profile.app.id, "playden-steam-220"));
    CHECK(lease.has_preset && lease.preset.bindings[0].kind != PW_GAME_BIND_UNSET);
    pw_title_publication_close(&lease); CHECK(lease.lock_fd == -1);
    PwTitlePublicationDescriptor kept, bad; memset(&bad, 0xa5, sizeof(bad)); kept = bad;
    CHECK(pw_title_publication_descriptor_parse((const uint8_t *)"{}", 2, "FAKE78001", &bad, NULL, 0) != 0);
    CHECK(!memcmp(&bad, &kept, sizeof(bad)));
    CHECK(pw_title_publication_read_descriptor(f.before.canonical_path, "FAKE78001",
          &f.platform, &bad, NULL, 0) == 0 && bad.length == f.before.length);
    fixture_destroy(&f);
}

static void refusal_contract(void)
{
    Fixture f; PwTitlePublicationLease lease; int calls = 0; char mapped[1200];
    fixture_init(&f, &calls);
    path(mapped, sizeof(mapped), &f, "/data/playden/games/install-220/.playden-control/publication.lock");
    int writer = open(mapped, O_RDONLY); CHECK(writer >= 0 && flock(writer, LOCK_EX | LOCK_NB) == 0);
    CHECK(admit(&f, &lease) == PW_TITLE_PUBLICATION_ADMISSION_BUSY && lease.lock_fd == -1);
    CHECK(close(writer) == 0);

    replace_once(f.receipt, sizeof(f.receipt), "\"phase\":\"committed\"", "\"phase\":\"pending\"");
    char publication_field[128];
    const char *publication = strstr(f.receipt, "\"publication_sha256\":\""); CHECK(publication);
    publication += strlen("\"publication_sha256\":\"");
    snprintf(publication_field, sizeof(publication_field), "\"publication_sha256\":\"%.*s\"", 64, publication);
    replace_once(f.receipt, sizeof(f.receipt), publication_field, "\"publication_sha256\":\"\"");
    write_file(&f, "/data/playden/games/install-220/.playden-control/publication-state.v1", f.receipt, strlen(f.receipt), 0600);
    CHECK(admit(&f, &lease) == PW_TITLE_PUBLICATION_ADMISSION_INVALID);
    fixture_refresh(&f);

    strcat(f.profile, "# changed\n");
    write_file(&f, "/data/playden/games/install-220/library/profiles/playden-steam-220.profile", f.profile, strlen(f.profile), 0600);
    CHECK(admit(&f, &lease) == PW_TITLE_PUBLICATION_ADMISSION_INVALID);
    fixture_refresh(&f);

    strcat(f.input, "# changed\n");
    write_file(&f, "/data/playden/games/install-220/library/input/gamepad.input", f.input, strlen(f.input), 0600);
    CHECK(admit(&f, &lease) == PW_TITLE_PUBLICATION_ADMISSION_INVALID);
    fixture_refresh(&f);

    strcat(f.descriptor, " ");
    write_file(&f, "/data/homebrew/FAKE78001/game-launch.json", f.descriptor, strlen(f.descriptor), 0600);
    CHECK(admit(&f, &lease) == PW_TITLE_PUBLICATION_ADMISSION_INVALID);
    fixture_destroy(&f);
}

static void filesystem_and_registry_contract(void)
{
    Fixture f; PwTitlePublicationLease lease; int calls = 0; char mapped[1200], text[32768];
    fixture_init(&f, &calls);
    path(mapped, sizeof(mapped), &f, "/data/playden/games/install-220/library/profiles");
    CHECK(chmod(mapped, 0755) == 0); CHECK(admit(&f, &lease) != 0); CHECK(chmod(mapped, 0700) == 0);
    fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    text[0] = 'X';
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    size_t registry_length = strlen(text);
    memcpy(text + registry_length, "\0junk\n", 6);
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text,
               registry_length + 6, 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "WINE REGISTRY Version 2\n",
                 "WINE REGISTRY Version 2\rgarbage\n");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "\"Version\"=\"win7\"", "\"Version\\x0000suffix\"=\"win7\"");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "\"Version\"=\"win7\"", "\"Version\"=\"WIN7\"");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "\"Version\"=\"win7\"\n", "\"Version\"=\"win7\"\n#link\n");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    strcat(text, "\n[Software\\\\Wine\\\\AppDefaults\\\\hl2.exe\\\\]\n#link\n");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    strcat(text, "\n[Software\\\\Wine\\\\AppDefaults\\\\hl2.exe\\\\]\n\"Other\"=\"x\"\n");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "hl2.exe]", "hl2.exe\\\\\\\\]");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "Desktop]", "Desktop\\\\\\\\\\\\]");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    strcat(text, "\n[Software]\n#link-target-is-irrelevant\n");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    const char *root_links[] = {"\n[]\n#link\n", "\n[\\x0000Software]\n#link\n",
                                "\n[\\\\]\n#link\n", NULL};
    for (const char **root_link = root_links; *root_link; ++root_link) {
        read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
        strcat(text, *root_link);
        write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
        CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);
    }

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    strcat(text, "\n[Software\\\\Wine\\\\UnrelatedLink]\n#link\n");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) == 0); pw_title_publication_close(&lease); fixture_refresh(&f);

    path(mapped, sizeof(mapped), &f, "/data/homebrew/FAKE78001/game-launch.json");
    CHECK(chmod(mapped, 0644) == 0); CHECK(admit(&f, &lease) != 0); CHECK(chmod(mapped, 0600) == 0);
    PwTitlePublicationPlatform saved = f.platform; f.platform.try_lock_shared = NULL;
    CHECK(admit(&f, &lease) != 0 && lease.lock_fd == -1);
    f.platform.try_lock_shared = unavailable_lock;
    CHECK(admit(&f, &lease) == PW_TITLE_PUBLICATION_ADMISSION_INVALID && lease.lock_fd == -1);
    f.platform = saved;
    path(mapped, sizeof(mapped), &f, "/data/playden/games/install-220/.playden-control/publication.lock");
    char alias[1200]; path(alias, sizeof(alias), &f, "/data/playden/games/install-220/.playden-control/lock-alias");
    CHECK(link(mapped, alias) == 0); CHECK(admit(&f, &lease) != 0); CHECK(unlink(alias) == 0);

    read_file(&f, "/data/playden/games/install-220/library/profiles/.playden-publication", text, sizeof(text));
    replace_once(text, sizeof(text), "schema=2", "schema=1");
    write_file(&f, "/data/playden/games/install-220/library/profiles/.playden-publication", text, strlen(text), 0600);
    publish_modified_metadata(&f); CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    path(mapped, sizeof(mapped), &f, "/data/homebrew/FAKE78001/games/220/dxvk.conf");
    CHECK(unlink(mapped) == 0);
    read_file(&f, "/data/playden/games/install-220/library/profiles/.playden-publication", text, sizeof(text));
    replace_once(text, sizeof(text), "dxvk.present=1", "dxvk.present=0");
    char *sha = strstr(text, "dxvk.sha256="); CHECK(sha); sha += strlen("dxvk.sha256=");
    char dxvk_line[80]; snprintf(dxvk_line, sizeof(dxvk_line), "dxvk.sha256=%.*s", 64, sha);
    replace_once(text, sizeof(text), dxvk_line, "dxvk.sha256=");
    write_file(&f, "/data/playden/games/install-220/library/profiles/.playden-publication", text, strlen(text), 0600);
    publish_modified_metadata(&f); CHECK(admit(&f, &lease) == 0); pw_title_publication_close(&lease);
    write_file(&f, "/data/homebrew/FAKE78001/games/220/dxvk.conf", "foreign", 7, 0644);
    CHECK(admit(&f, &lease) != 0); fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "\"Version\"=\"win7\"\n", "");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    read_file(&f, "/data/playden/games/install-220/library/profiles/.playden-publication", text, sizeof(text));
    replace_once(text, sizeof(text), "registry.0.kind=string", "registry.0.kind=absent");
    replace_once(text, sizeof(text), "registry.0.string_hex=77696e37", "registry.0.string_hex=");
    write_file(&f, "/data/playden/games/install-220/library/profiles/.playden-publication", text, strlen(text), 0600);
    publish_modified_metadata(&f); CHECK(admit(&f, &lease) == 0); pw_title_publication_close(&lease);
    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text),
        "\n\n[Software\\\\Wine\\\\Unrelated]",
        "\n\"V\xc0\x80" "ersion\"=\"win7\"\n\n[Software\\\\Wine\\\\Unrelated]");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0);
    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "\"V\xc0\x80" "ersion\"", "\"V\\\xc0\x80" "ersion\"");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0);
    fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    replace_once(text, sizeof(text), "\"Version\"=\"win7\"", "  \"Version\" \t= \t\"win7\"");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) == 0); pw_title_publication_close(&lease);
    strcat(text,
        "\n \t[\\x0053oftware\\\\Wine\\\\AppDefaults\\\\hl2.exe]\n"
        "  \"\\126ersion\" = \"win7\"\n");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0);
    fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    char *value = strstr(text, "00000078"); CHECK(value); memcpy(value, "00000060", 8);
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0644);
    CHECK(admit(&f, &lease) != 0);
    fixture_refresh(&f);

    read_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, sizeof(text));
    strcat(text, "\n[control panel\\\\desktop]\n\"logpixels\"=dword:00000078\n");
    write_file(&f, "/data/playden/games/install-220/library/prefixes/game/user.reg", text, strlen(text), 0600);
    CHECK(admit(&f, &lease) != 0);
    fixture_refresh(&f);

    unlink(mapped);
    path(mapped, sizeof(mapped), &f, "/data/playden/games/install-220/library/profiles/playden-steam-220.profile");
    CHECK(unlink(mapped) == 0 && symlink("profiles.lst", mapped) == 0);
    CHECK(admit(&f, &lease) != 0);
    fixture_destroy(&f);
}

static void no_preset_contract(void)
{
    Fixture f; PwTitlePublicationLease lease; PwGameInput empty; int calls = 0;
    fixture_init(&f, &calls);
    char *input = strstr(f.profile, "\n[input]\n"); CHECK(input); *input = 0;
    fixture_refresh(&f);
    CHECK(admit(&f, &lease) == 0 && !lease.has_preset);
    pw_game_input_init(&empty); CHECK(!memcmp(&lease.preset, &empty, sizeof(empty)));
    pw_title_publication_close(&lease); fixture_destroy(&f);
}

static void cross_process_lock_contract(void)
{
    Fixture f; PwTitlePublicationLease lease; int calls = 0; int ready[2]; char mapped[1200], byte;
    fixture_init(&f, &calls); CHECK(pipe(ready) == 0);
    path(mapped, sizeof(mapped), &f, "/data/playden/games/install-220/.playden-control/publication.lock");
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(ready[0]); int fd = open(mapped, O_RDONLY); if (fd < 0 || flock(fd, LOCK_EX) != 0) _exit(2);
        if (write(ready[1], "x", 1) != 1) _exit(3); sleep(1); close(fd); _exit(0);
    }
    close(ready[1]); CHECK(read(ready[0], &byte, 1) == 1); close(ready[0]);
    CHECK(admit(&f, &lease) == PW_TITLE_PUBLICATION_ADMISSION_BUSY);
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(admit(&f, &lease) == 0); pw_title_publication_close(&lease);
    fixture_destroy(&f);
}

static void publisher_verification_contract(void)
{
    Fixture f; int calls = 0; fixture_init(&f, &calls); char mapped[1200], digest_text[65], error[256];
    path(mapped, sizeof(mapped), &f, "/data/playden/games/install-220/.playden-control/publication.lock");
    int writer = open(mapped, O_RDONLY); CHECK(writer >= 0 && flock(writer, LOCK_EX | LOCK_NB) == 0);
    digest(f.metadata, strlen(f.metadata), digest_text);
    PwTitlePublicationContents contents, sentinel; memset(&contents, 0xa5, sizeof(contents)); sentinel = contents;
    f.platform.context = &calls;
    CHECK(pw_title_publication_verify_locked(&f.before, &f.platform, writer,
          (const uint8_t *)f.receipt, strlen(f.receipt), digest_text, &contents, error, sizeof(error)) == 0);
    CHECK(calls == 0 && !strcmp(contents.profile.app.id, "playden-steam-220") && contents.has_preset);
    PwTitlePublicationPlatform no_shared = f.platform; no_shared.try_lock_shared = NULL;
    CHECK(pw_title_publication_verify_locked(&f.before, &no_shared, writer,
          (const uint8_t *)f.receipt, strlen(f.receipt), digest_text, &contents, error, sizeof(error)) == 0);
    char foreign_path[1200]; path(foreign_path, sizeof(foreign_path), &f,
        "/data/playden/games/install-220/library/profiles/playden-steam-220.profile");
    int foreign = open(foreign_path, O_RDONLY); CHECK(foreign >= 0); contents = sentinel;
    CHECK(pw_title_publication_verify_locked(&f.before, &f.platform, foreign,
          (const uint8_t *)f.receipt, strlen(f.receipt), digest_text, &contents, error, sizeof(error)) != 0);
    CHECK(!memcmp(&contents, &sentinel, sizeof(contents)) && fcntl(foreign, F_GETFD) >= 0);
    CHECK(close(foreign) == 0);
    PwTitlePublicationLease reader; CHECK(admit(&f, &reader) == PW_TITLE_PUBLICATION_ADMISSION_BUSY);
    CHECK(fcntl(writer, F_GETFD) >= 0); // Verification did not release/close the caller's lease.
    char pending[8192]; strcpy(pending, f.receipt);
    replace_once(pending, sizeof(pending), "\"phase\":\"committed\"", "\"phase\":\"pending\"");
    char *hash = strstr(pending, "\"publication_sha256\":\""); CHECK(hash); hash += strlen("\"publication_sha256\":\"");
    memmove(hash, hash + 64, strlen(hash + 64) + 1);
    write_file(&f, "/data/playden/games/install-220/.playden-control/publication-state.v1", pending, strlen(pending), 0600);
    CHECK(pw_title_publication_verify_locked(&f.before, &f.platform, writer,
          (const uint8_t *)pending, strlen(pending), digest_text, &contents, error, sizeof(error)) == 0);
    CHECK(calls == 1); // Only the separate reader attempted a shared lock.
    contents = sentinel;
    CHECK(pw_title_publication_verify_locked(&f.before, &f.platform, writer,
          (const uint8_t *)f.receipt, strlen(f.receipt), digest_text, &contents, error, sizeof(error)) != 0);
    CHECK(!memcmp(&contents, &sentinel, sizeof(contents)) && fcntl(writer, F_GETFD) >= 0);
    digest_text[0] = digest_text[0] == '0' ? '1' : '0';
    CHECK(pw_title_publication_verify_locked(&f.before, &f.platform, writer,
          (const uint8_t *)pending, strlen(pending), digest_text, &contents, error, sizeof(error)) != 0);
    CHECK(!memcmp(&contents, &sentinel, sizeof(contents)));
    digest(f.metadata, strlen(f.metadata), digest_text);
    write_file(&f, "/data/playden/games/install-220/library/input/gamepad.input", "[input]\nmode=keyboard\n", 22, 0600);
    CHECK(pw_title_publication_verify_locked(&f.before, &f.platform, writer,
          (const uint8_t *)pending, strlen(pending), digest_text, &contents, error, sizeof(error)) != 0);
    CHECK(!memcmp(&contents, &sentinel, sizeof(contents)));
    CHECK(close(writer) == 0); fixture_refresh(&f);
    CHECK(admit(&f, &reader) == 0); pw_title_publication_close(&reader);
    fixture_destroy(&f);
}
static void explicit_download_root_contract(void) {
    const char *roots[] = {"/data/homebrew/FAKE77031/games/220", "/mnt/usb0/playden/games/220", NULL};
    for (unsigned i = 0; roots[i]; ++i) {
        Fixture f; int calls = 0; fixture_init(&f, &calls);
        strcpy(f.game_root, roots[i]); directory(&f, f.game_root, 0755);
        char suffix[1200]; CHECK(snprintf(suffix, sizeof(suffix), ",\"game_root\":\"%s\"}\n", f.game_root) > 0);
        replace_once(f.descriptor, sizeof(f.descriptor), "\"schema_version\":1", "\"schema_version\":2");
        replace_once(f.descriptor, sizeof(f.descriptor), "}\n", suffix);
        char windows[1100]; CHECK(snprintf(windows, sizeof(windows), "Z:%s", f.game_root) > 0);
        for (char *p = windows; *p; ++p) if (*p == '/') *p = '\\';
        replace_once(f.profile, sizeof(f.profile), "Z:\\data\\homebrew\\FAKE78001\\games\\220", windows);
        replace_once(f.profile, sizeof(f.profile), "Z:\\data\\homebrew\\FAKE78001\\games\\220", windows);
        fixture_refresh(&f);
        CHECK(pw_title_publication_descriptor_parse((const uint8_t *)f.descriptor, strlen(f.descriptor), "FAKE78001", &f.before, NULL, 0) == PW_OK);
        strcpy(f.before.launch.game_root, "/data/unrelated/220"); // Bind parsed on-disk bytes, not a modified observation field.
        PwTitlePublicationLease lease; CHECK(admit(&f, &lease) == PW_OK); pw_title_publication_close(&lease);
        // Rehashed profile bytes outside the explicit root still fail semantics.
        replace_once(f.profile, sizeof(f.profile), windows, "Z:\\data\\unrelated\\220");
        fixture_refresh(&f); CHECK(admit(&f, &lease) != PW_OK);
        fixture_destroy(&f);
    }
}

static int metadata_parses(const char *text)
{
    PwTitlePublicationMetadata parsed;
    return pw_title_publication_metadata_parse((const uint8_t *)text, strlen(text), &parsed) == PW_OK;
}

/* A game folder on a filesystem without owners or permissions (exFAT) is
 * admitted only when schema 3 metadata says so; private files stay strict. */
static void unowned_game_root_contract(void)
{
    Fixture f; PwTitlePublicationLease lease; int calls = 0;
    char game[1200], dxvk[1200], profiles[1200], alias[1200], text[32768], variant[32768];
    const char *metadata = "/data/playden/games/install-220/library/profiles/.playden-publication";
    fixture_init(&f, &calls);
    path(game, sizeof(game), &f, "/data/homebrew/FAKE78001/games/220");
    path(dxvk, sizeof(dxvk), &f, "/data/homebrew/FAKE78001/games/220/dxvk.conf");
    CHECK(chmod(game, 0777) == 0); CHECK(admit(&f, &lease) != 0);
    CHECK(chmod(game, 0755) == 0); CHECK(chmod(dxvk, 0666) == 0); CHECK(admit(&f, &lease) != 0);

    read_file(&f, metadata, text, sizeof(text));
    strcpy(variant, text); replace_once(variant, sizeof(variant), "schema=2", "schema=3");
    CHECK(!metadata_parses(variant));                       /* schema 3 states the field */
    strcat(variant, "game_root.unowned=2\n"); CHECK(!metadata_parses(variant));
    strcpy(variant, text); strcat(variant, "game_root.unowned=1\n");
    CHECK(!metadata_parses(variant));                       /* schema 2 never has it */
    replace_once(variant, sizeof(variant), "schema=2", "schema=3"); CHECK(metadata_parses(variant));
    strcat(variant, "game_root.unowned=1\n"); CHECK(!metadata_parses(variant));  /* duplicate */

    strcpy(variant, text); replace_once(variant, sizeof(variant), "schema=2", "schema=3");
    strcat(variant, "game_root.unowned=0\n");
    write_file(&f, metadata, variant, strlen(variant), 0600);
    publish_modified_metadata(&f); CHECK(chmod(dxvk, 0666) == 0); CHECK(admit(&f, &lease) != 0);

    replace_once(variant, sizeof(variant), "game_root.unowned=0", "game_root.unowned=1");
    write_file(&f, metadata, variant, strlen(variant), 0600);
    publish_modified_metadata(&f);
    CHECK(chmod(game, 0777) == 0); CHECK(chmod(dxvk, 0666) == 0);
    CHECK(admit(&f, &lease) == 0); pw_title_publication_close(&lease);
    /* Still refused: a hard-linked dxvk.conf and a loosened private folder. */
    CHECK(snprintf(alias, sizeof(alias), "%s.alias", dxvk) > 0);
    CHECK(link(dxvk, alias) == 0); CHECK(admit(&f, &lease) != 0); CHECK(unlink(alias) == 0);
    path(profiles, sizeof(profiles), &f, "/data/playden/games/install-220/library/profiles");
    CHECK(chmod(profiles, 0755) == 0); CHECK(admit(&f, &lease) != 0); CHECK(chmod(profiles, 0700) == 0);
    CHECK(admit(&f, &lease) == 0); pw_title_publication_close(&lease);
    CHECK(chmod(game, 0755) == 0);
    fixture_destroy(&f);
}

int main(void)
{
    explicit_download_root_contract();
    publisher_verification_contract();
    valid_and_descriptor_contract();
    refusal_contract();
    filesystem_and_registry_contract();
    no_preset_contract();
    cross_process_lock_contract();
    unowned_game_root_contract();
    puts("test_pw_title_publication_native: passed");
    return 0;
}
