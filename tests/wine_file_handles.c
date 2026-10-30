/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Many files can be kept open and read at once (patch 0993). Unreal keeps
 * every .pak/.utoc/.ucas or .upk it mounts open, and on the console each
 * open handle held a file the title can have only about 256 of. This opens
 * 1500 files read-only, keeps them all open and reads each one three times,
 * through the file pointer and at explicit offsets, so that a server that
 * parks their descriptors has to open them again with the right position.
 * Then a file is renamed and another deleted (POSIX semantics: its name goes
 * at once) while read-only handles to them are parked, and both handles must
 * still read their file. Last, a file with 300 read-only handles, most of
 * them parked, is deleted: every handle must be opened again first, so the
 * delete may be refused when they don't all fit, but each handle must still
 * read the file afterwards. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

NTSTATUS NTAPI NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);

#define COUNT 1500
#define READERS 300
#define FILE_DISPOSITION_INFORMATION_EX_CLASS 64  /* FileDispositionInformationEx */
#define SIZE  4096

static HANDLE files[COUNT];
static HANDLE readers[READERS];
static WCHAR top[MAX_PATH];

static BYTE expected(int file, int offset)
{
    return (BYTE)(file * 7 + offset);
}

static void path_of(WCHAR *path, const WCHAR *name)
{
    swprintf(path, MAX_PATH, L"%ls\\%ls", top, name);
}

static BOOL delete_posix(const WCHAR *path, ULONG *status)
{
    IO_STATUS_BLOCK io;
    ULONG disposition = 0x1 | 0x2;  /* FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS */
    HANDLE deleter = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                 OPEN_EXISTING, 0, NULL);

    if (deleter == INVALID_HANDLE_VALUE) return FALSE;
    *status = NtSetInformationFile(deleter, &io, &disposition, sizeof(disposition),
                                   (FILE_INFORMATION_CLASS)FILE_DISPOSITION_INFORMATION_EX_CLASS);
    CloseHandle(deleter);
    return TRUE;
}

static BOOL write_file(const WCHAR *name, int file)
{
    static BYTE data[SIZE];
    WCHAR path[MAX_PATH];
    HANDLE handle;
    DWORD done;
    int i;

    for (i = 0; i < SIZE; i++) data[i] = expected(file, i);
    path_of(path, name);
    handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    if (!WriteFile(handle, data, SIZE, &done, NULL) || done != SIZE) done = 0;
    CloseHandle(handle);
    return done == SIZE;
}

static HANDLE open_read(const WCHAR *name)
{
    WCHAR path[MAX_PATH];

    path_of(path, name);
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, 0, NULL);
}

/* read count bytes at the file pointer (offset < 0) or at offset, and
 * check them against the file's pattern starting at check_offset */
static BOOL read_check(HANDLE handle, int file, int offset, int check_offset, int count)
{
    BYTE data[64];
    OVERLAPPED ov;
    DWORD done = 0;
    int i;

    memset(&ov, 0, sizeof(ov));
    ov.Offset = offset;
    if (!ReadFile(handle, data, count, &done, offset < 0 ? NULL : &ov) || done != (DWORD)count) return FALSE;
    for (i = 0; i < count; i++)
        if (data[i] != expected(file, check_offset + i)) return FALSE;
    return TRUE;
}

/* use every other file, so that a handle not used since is parked */
static BOOL touch_all(void)
{
    int i;

    for (i = 0; i < COUNT; i++)
        if (!read_check(files[i], i, 2000, 2000, 4)) return FALSE;
    return TRUE;
}

static int fail(const char *what, int index)
{
    printf("file-handles failed=%s index=%d error=%lu\n", what, index, GetLastError());
    return 1;
}

int main(void)
{
    WCHAR name[64], path[MAX_PATH], renamed[MAX_PATH];
    HANDLE moved, deleted;
    ULONG status;
    int i;

    GetTempPathW(MAX_PATH, path);
    swprintf(top, MAX_PATH, L"%lsfile-handles-%lu-%llu", path, GetCurrentProcessId(), GetTickCount64());
    if (!CreateDirectoryW(top, NULL)) return fail("mkdir", 0);
    for (i = 0; i < COUNT + 3; i++)
    {
        swprintf(name, 64, L"f%d.bin", i);
        if (!write_file(name, i)) return fail("create", i);
    }

    for (i = 0; i < COUNT; i++)
    {
        swprintf(name, 64, L"f%d.bin", i);
        if ((files[i] = open_read(name)) == INVALID_HANDLE_VALUE) return fail("open", i);
    }
    /* through the file pointer: 0..5, then 6..15 in reverse order */
    for (i = 0; i < COUNT; i++)
        if (!read_check(files[i], i, -1, 0, 6)) return fail("read-pointer", i);
    for (i = COUNT - 1; i >= 0; i--)
        if (!read_check(files[i], i, -1, 6, 10)) return fail("read-pointer-again", i);
    /* a moved file pointer and an explicit offset */
    for (i = 0; i < COUNT; i++)
        if (SetFilePointer(files[i], 1000, NULL, FILE_BEGIN) != 1000) return fail("seek", i);
    for (i = 0; i < COUNT; i++)
        if (!read_check(files[i], i, -1, 1000, 8) || !read_check(files[i], i, 3000, 3000, 8))
            return fail("read-seek", i);
    /* a read at an explicit offset moves the pointer of a synchronous handle */
    for (i = 0; i < COUNT; i++)
        if (SetFilePointer(files[i], 0, NULL, FILE_CURRENT) != 3008) return fail("tell", i);
    for (i = 0; i < COUNT; i++)
        if (!read_check(files[i], i, -1, 3008, 8)) return fail("read-after-tell", i);

    /* rename a file whose read-only handle is parked: it still reads the file */
    swprintf(name, 64, L"f%d.bin", COUNT);
    if ((moved = open_read(name)) == INVALID_HANDLE_VALUE) return fail("open-moved", 0);
    if (!read_check(moved, COUNT, -1, 0, 4)) return fail("read-moved", 0);
    if (!touch_all()) return fail("touch", 0);
    path_of(path, name);
    path_of(renamed, L"renamed.bin");
    if (!MoveFileW(path, renamed)) return fail("rename", 0);
    if (!touch_all()) return fail("touch", 1);
    if (!read_check(moved, COUNT, -1, 4, 4)) return fail("read-after-rename", 0);

    /* delete a file whose read-only handle is parked: it still reads it */
    swprintf(name, 64, L"f%d.bin", COUNT + 1);
    if ((deleted = open_read(name)) == INVALID_HANDLE_VALUE) return fail("open-deleted", 0);
    if (!read_check(deleted, COUNT + 1, -1, 0, 4)) return fail("read-deleted", 0);
    if (!touch_all()) return fail("touch", 2);
    path_of(path, name);
    if (!delete_posix(path, &status) || status) return fail("delete", (int)status);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return fail("still-there", 0);
    if (!touch_all()) return fail("touch", 3);
    if (!read_check(deleted, COUNT + 1, -1, 4, 4)) return fail("read-after-delete", 0);

    /* many handles to one file, most of them parked, then the file deleted */
    swprintf(name, 64, L"f%d.bin", COUNT + 2);
    for (i = 0; i < READERS; i++)
    {
        if ((readers[i] = open_read(name)) == INVALID_HANDLE_VALUE) return fail("open-reader", i);
        if (!read_check(readers[i], COUNT + 2, -1, 0, 4)) return fail("read-reader", i);
    }
    if (!touch_all()) return fail("touch", 4);
    path_of(path, name);
    if (!delete_posix(path, &status)) return fail("open-multi-deleter", 0);
    for (i = 0; i < READERS; i++)
        if (!read_check(readers[i], COUNT + 2, -1, 4, 4)) return fail("read-reader-after-delete", i);
    for (i = 0; i < READERS; i++) CloseHandle(readers[i]);
    DeleteFileW(path);
    printf("file-handles multi-reader delete status=%#lx\n", status);

    CloseHandle(moved);
    CloseHandle(deleted);
    for (i = 0; i < COUNT; i++) CloseHandle(files[i]);
    for (i = 0; i < COUNT; i++)
    {
        swprintf(name, 64, L"f%d.bin", i);
        path_of(path, name);
        DeleteFileW(path);
    }
    DeleteFileW(renamed);
    RemoveDirectoryW(top);
    printf("file-handles verdict=pass files=%d\n", COUNT);
    return 0;
}
