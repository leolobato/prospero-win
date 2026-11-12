/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_steam_view.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int valid(const char *name) { return pw_steam_install_dir_valid(name, strlen(name)); }

static void test_install_dir(void)
{
    char long_name[PW_STEAM_INSTALL_DIR_CAPACITY + 1];

    assert(valid("The Talos Principle"));
    assert(valid("Half-Life 2"));
    assert(valid("Baldur's Gate 3 & More (Beta)"));
    assert(valid("Stray"));
    assert(valid("\xc3\x89t\xc3\xa9"));        /* UTF-8 */
    assert(valid(".hidden"));
    assert(!valid(""));
    assert(!valid(".") && !valid(".."));
    assert(!valid("a\\b") && !valid("a/b") && !valid("C:") && !valid("a*") && !valid("a?") &&
           !valid("a\"b") && !valid("a<b") && !valid("a>b") && !valid("a|b"));
    assert(!valid("tab\there") && !valid("line\nbreak") && !valid("del\x7f"));
    assert(!valid("trailing ") && !valid("trailing."));
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    assert(!valid(long_name));
    long_name[PW_STEAM_INSTALL_DIR_CAPACITY - 1] = 0;
    assert(valid(long_name));
}

static void test_root(void)
{
    char out[260], small[20];

    assert(!pw_steam_view_root("The Talos Principle", out, sizeof(out)));
    assert(!strcmp(out, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Talos Principle"));
    assert(pw_steam_view_root("The Talos Principle", small, sizeof(small)) == -1 && !small[0]);
    assert(pw_steam_view_root("..", out, sizeof(out)) == -1);
}

static void test_map(void)
{
    const char *z = "Z:\\mnt\\usb1\\playden\\games\\257510";
    const char *c = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Talos Principle";
    char out[260], back[260], small[20];

    /* The program and its folder, to Wine's view and back. */
    assert(pw_steam_view_map(z, c, "Z:\\mnt\\usb1\\playden\\games\\257510\\Bin\\x64\\Talos.exe", out, sizeof(out)) == 1);
    assert(!strcmp(out, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Talos Principle\\Bin\\x64\\Talos.exe"));
    assert(pw_steam_view_map(c, z, out, back, sizeof(back)) == 1);
    assert(!strcmp(back, "Z:\\mnt\\usb1\\playden\\games\\257510\\Bin\\x64\\Talos.exe"));
    /* The root itself. */
    assert(pw_steam_view_map(z, c, z, out, sizeof(out)) == 1 && !strcmp(out, c));
    /* Windows paths ignore case: a launcher may spell the folder its own way. */
    assert(pw_steam_view_map(c, z, "c:\\program files (x86)\\steam\\steamapps\\common\\the talos principle\\Talos.exe",
                             out, sizeof(out)) == 1);
    assert(!strcmp(out, "Z:\\mnt\\usb1\\playden\\games\\257510\\Talos.exe"));
    /* A sibling whose name starts the same, and another drive, stay as they are. */
    assert(pw_steam_view_map(z, c, "Z:\\mnt\\usb1\\playden\\games\\2575100\\a.exe", out, sizeof(out)) == 0);
    assert(!strcmp(out, "Z:\\mnt\\usb1\\playden\\games\\2575100\\a.exe"));
    assert(pw_steam_view_map(z, c, "C:\\windows\\system32\\cmd.exe", out, sizeof(out)) == 0);
    assert(!strcmp(out, "C:\\windows\\system32\\cmd.exe"));
    assert(pw_steam_view_map(z, c, "Z:\\mnt", out, sizeof(out)) == 0 && !strcmp(out, "Z:\\mnt"));
    /* Too long either way: refused, nothing half written. */
    assert(pw_steam_view_map(z, c, z, small, sizeof(small)) == -1 && !small[0]);
    assert(pw_steam_view_map(z, c, "Z:\\elsewhere\\with\\a\\long\\path.exe", small, sizeof(small)) == -1 && !small[0]);
    assert(pw_steam_view_map("", c, z, out, sizeof(out)) == 0);
    /* Talos is not Talos2, either way; a trailing separator is the root's folder. */
    assert(pw_steam_view_map(c, z, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Talos Principle2\\a.exe",
                             out, sizeof(out)) == 0);
    assert(pw_steam_view_map(c, z, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Talos Principle\\",
                             out, sizeof(out)) == 1 && !strcmp(out, "Z:\\mnt\\usb1\\playden\\games\\257510\\"));
    /* Exactly fitting a 260-byte buffer (259 characters), and one more. */
    {
        char path[300], fitted[260];
        int head = snprintf(path, sizeof(path), "%s\\", z);
        memset(path + head, 'a', 259 - strlen(c) - 1);
        path[head + 259 - strlen(c) - 1] = 0;
        assert(pw_steam_view_map(z, c, path, fitted, sizeof(fitted)) == 1 && strlen(fitted) == 259);
        strcat(path, "a");
        assert(pw_steam_view_map(z, c, path, fitted, sizeof(fitted)) == -1 && !fitted[0]);
    }
}

static void test_table(void)
{
    char out[PW_STEAM_TABLE_MAX], small[16];
    const char *other = "257510\t/mnt/usb1/playden/games/257510\n";
    long length;

    /* A new table. */
    length = pw_steam_view_table(NULL, 0, "The Talos Principle", "/mnt/usb1/playden/games/257510", out, sizeof(out));
    assert(length == (long)strlen(out) && !strcmp(out, "The Talos Principle\t/mnt/usb1/playden/games/257510\n"));
    /* The drive moved: the game's line and an earlier name of it (the app ID
     * before Steam's folder name was known) both point at the new drive. */
    {
        char existing[256];
        snprintf(existing, sizeof(existing), "%sThe Talos Principle\t/mnt/usb1/playden/games/257510\n", other);
        length = pw_steam_view_table(existing, strlen(existing), "The Talos Principle",
                                     "/mnt/usb0/playden/games/257510", out, sizeof(out));
        assert(length > 0 && !strcmp(out, "257510\t/mnt/usb0/playden/games/257510\n"
                                          "The Talos Principle\t/mnt/usb0/playden/games/257510\n"));
        /* A new name joins the ones there, last. */
        length = pw_steam_view_table(other, strlen(other), "The Talos Principle", "/mnt/usb1/playden/games/257510",
                                     out, sizeof(out));
        assert(length > 0 && !strcmp(out, "257510\t/mnt/usb1/playden/games/257510\n"
                                          "The Talos Principle\t/mnt/usb1/playden/games/257510\n"));
        /* Unchanged: the same bytes, so the title need not write it. */
        length = pw_steam_view_table(out, strlen(out), "The Talos Principle", "/mnt/usb1/playden/games/257510",
                                     existing, sizeof(existing));
        assert(length == (long)strlen(out) && !memcmp(existing, out, (size_t)length));
        /* A last line without its newline still counts. */
        length = pw_steam_view_table("A\t/a", 4, "B", "/b", out, sizeof(out));
        assert(length > 0 && !strcmp(out, "A\t/b\nB\t/b\n"));
    }
    /* Malformed tables and values are refused rather than guessed at. */
    assert(pw_steam_view_table("no tab\n", 7, "B", "/b", out, sizeof(out)) == -1);
    assert(pw_steam_view_table("\t/b\n", 4, "B", "/b", out, sizeof(out)) == -1);
    assert(pw_steam_view_table("A\t\n", 3, "B", "/b", out, sizeof(out)) == -1);
    assert(pw_steam_view_table(NULL, 0, "B", "/b\nC\t/c", out, sizeof(out)) == -1);
    assert(pw_steam_view_table(NULL, 0, "B", "", out, sizeof(out)) == -1);
    assert(pw_steam_view_table(NULL, 0, "a/b", "/b", out, sizeof(out)) == -1);
    assert(pw_steam_view_table(NULL, 0, "The Talos Principle", "/mnt/usb1/x", small, sizeof(small)) == -1);
}

int main(void)
{
    test_install_dir();
    test_root();
    test_map();
    test_table();
    puts("steam view passed: install_dir names, the steamapps root, paths to Wine's view and back, the link table");
    return 0;
}
