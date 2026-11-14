/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Many files can be kept open for writing, and locked, at once. A program
 * that opens a SQLite database once per thread holds one read-write handle
 * per connection, and in WAL mode each connection keeps a shared lock on the
 * database for as long as it is open; Java code keeps region and save files
 * open for writing. On the console each of those handles held one of the
 * title's ~256 open files, and patch 0993 parked only read-only files that no
 * handle had locked.
 *
 * This opens one database file 300 times for reading and writing, takes a
 * shared lock through every handle as SQLite does, and writes and reads each
 * handle's own slot at explicit offsets. It writes 400 more files through
 * their file pointers in rounds, so a server that parks them has to open them
 * again for writing at the right position. 40 files are created with the
 * read-only attribute but opened for writing, which a handle may still write
 * to although the file can't be opened for writing again. A file mapped for
 * writing must keep one view of its data through the mapping and the handle.
 * Then an exclusive lock through a new handle must still conflict with the
 * shared locks of the parked handles, and with a lock only a handle past the
 * cached ones holds, also after another handle of the file is closed. Last,
 * eight threads append records through their file pointers while two others
 * keep opening files, and every record must be where it was written. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CONNS 300
#define SLOT 128
#define WRITERS 400
#define READONLY 40
#define CHUNK 256
#define ROUNDS 4
#define SHARED_FIRST 0x40000002  /* SQLite's shared lock range on Windows */
#define SHARED_SIZE 510
#define LONE_LOCK 0x50000000     /* a byte only the last connection locks */
#define STRESS_THREADS 8
#define STRESS_RECORDS 400
#define RECORD 64

static HANDLE conns[CONNS];
static HANDLE writers[WRITERS + READONLY];
static WCHAR top[MAX_PATH];

static void path_of(WCHAR *path, const WCHAR *name)
{
    swprintf(path, MAX_PATH, L"%ls\\%ls", top, name);
}

static int fail(const char *what, int index)
{
    printf("written-files verdict=fail step=%s index=%d error=%lu\n", what, index, GetLastError());
    return 1;
}

static void fill(BYTE *data, int size, int file, int round)
{
    int i;
    for (i = 0; i < size; i++) data[i] = (BYTE)(file * 13 + round * 7 + i);
}

static BOOL write_at(HANDLE handle, DWORD offset, const BYTE *data, DWORD size)
{
    OVERLAPPED ov = {0};
    DWORD done = 0;

    ov.Offset = offset;
    return WriteFile(handle, data, size, &done, &ov) && done == size;
}

static BOOL read_at(HANDLE handle, DWORD offset, BYTE *data, DWORD size)
{
    OVERLAPPED ov = {0};
    DWORD done = 0;

    ov.Offset = offset;
    return ReadFile(handle, data, size, &done, &ov) && done == size;
}

static BOOL lock_shared(HANDLE handle)
{
    OVERLAPPED ov = {0};

    ov.Offset = SHARED_FIRST;
    return LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY, 0, SHARED_SIZE, 0, &ov);
}

static BOOL lock_exclusive(HANDLE handle)
{
    OVERLAPPED ov = {0};

    ov.Offset = SHARED_FIRST;
    return LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY | LOCKFILE_EXCLUSIVE_LOCK, 0, SHARED_SIZE, 0, &ov);
}

static BOOL lock_byte(HANDLE handle, DWORD offset, BOOL exclusive)
{
    OVERLAPPED ov = {0};

    ov.Offset = offset;
    return LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &ov);
}

static BOOL unlock_byte(HANDLE handle, DWORD offset)
{
    OVERLAPPED ov = {0};

    ov.Offset = offset;
    return UnlockFileEx(handle, 0, 1, 0, &ov);
}

static BOOL unlock(HANDLE handle)
{
    OVERLAPPED ov = {0};

    ov.Offset = SHARED_FIRST;
    return UnlockFileEx(handle, 0, SHARED_SIZE, 0, &ov);
}

static HANDLE open_rw(const WCHAR *name, DWORD disposition, DWORD attributes)
{
    WCHAR path[MAX_PATH];

    path_of(path, name);
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       disposition, attributes, NULL);
}

/* each connection writes its slot, then reads it back through the next one */
static int check_slots(int round)
{
    BYTE data[SLOT], back[SLOT];
    int i;

    for (i = 0; i < CONNS; i++)
    {
        fill(data, SLOT, i, round);
        if (!write_at(conns[i], i * SLOT, data, SLOT)) return fail("write-slot", i);
    }
    for (i = 0; i < CONNS; i++)
    {
        fill(data, SLOT, i, round);
        if (!read_at(conns[(i + 1) % CONNS], i * SLOT, back, SLOT)) return fail("read-slot", i);
        if (memcmp(data, back, SLOT)) return fail("slot-data", i);
    }
    return 0;
}

/* Writers append records through their file pointers while other threads
 * open and read files, so the server keeps parking files and opening them
 * again: a file parked while its writer held a descriptor would lose its
 * position, and a later record would overwrite an earlier one. */
static HANDLE stress_files[STRESS_THREADS];
static volatile LONG stress_done;

static DWORD WINAPI stress_writer(void *arg)
{
    int index = (int)(INT_PTR)arg, i;
    BYTE record[RECORD];
    DWORD done;

    for (i = 0; i < STRESS_RECORDS; i++)
    {
        memset(record, 0, sizeof(record));
        sprintf((char *)record, "thread %d record %d", index, i);
        if (!WriteFile(stress_files[index], record, RECORD, &done, NULL) || done != RECORD) return 1;
    }
    return 0;
}

static DWORD WINAPI stress_churn(void *arg)
{
    int i = (int)(INT_PTR)arg;
    WCHAR name[64], path[MAX_PATH];
    BYTE byte;
    DWORD done;

    while (!stress_done)
    {
        HANDLE file;

        swprintf(name, 64, L"w%04d.bin", i++ % WRITERS);
        path_of(path, name);
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE) continue;
        ReadFile(file, &byte, 1, &done, NULL);
        CloseHandle(file);
    }
    return 0;
}

static int stress(void)
{
    HANDLE writers_threads[STRESS_THREADS], churn[2];
    BYTE record[RECORD], expect[RECORD];
    WCHAR name[64];
    DWORD done, code;
    int i, j;

    for (i = 0; i < STRESS_THREADS; i++)
    {
        swprintf(name, 64, L"stress%d.bin", i);
        if ((stress_files[i] = open_rw(name, CREATE_NEW, 0)) == INVALID_HANDLE_VALUE) return fail("create-stress", i);
    }
    for (i = 0; i < 2; i++) churn[i] = CreateThread(NULL, 0, stress_churn, (void *)(INT_PTR)(i * 200), 0, NULL);
    for (i = 0; i < STRESS_THREADS; i++)
        writers_threads[i] = CreateThread(NULL, 0, stress_writer, (void *)(INT_PTR)i, 0, NULL);
    WaitForMultipleObjects(STRESS_THREADS, writers_threads, TRUE, INFINITE);
    stress_done = 1;
    WaitForMultipleObjects(2, churn, TRUE, INFINITE);
    for (i = 0; i < STRESS_THREADS; i++)
    {
        if (!GetExitCodeThread(writers_threads[i], &code) || code) return fail("stress-write", i);
        if (GetFileSize(stress_files[i], NULL) != STRESS_RECORDS * RECORD) return fail("stress-size", i);
        if (SetFilePointer(stress_files[i], 0, NULL, FILE_BEGIN) != 0) return fail("stress-rewind", i);
        for (j = 0; j < STRESS_RECORDS; j++)
        {
            memset(expect, 0, sizeof(expect));
            sprintf((char *)expect, "thread %d record %d", i, j);
            if (!ReadFile(stress_files[i], record, RECORD, &done, NULL) || done != RECORD) return fail("stress-read", i);
            if (memcmp(record, expect, RECORD)) return fail("stress-record", i * 10000 + j);
        }
        CloseHandle(writers_threads[i]);
        CloseHandle(stress_files[i]);
        swprintf(name, 64, L"stress%d.bin", i);
        {
            WCHAR path[MAX_PATH];
            path_of(path, name);
            DeleteFileW(path);
        }
    }
    for (i = 0; i < 2; i++) CloseHandle(churn[i]);
    return 0;
}

int main(void)
{
    BYTE data[CHUNK * ROUNDS], back[CHUNK * ROUNDS];
    WCHAR name[64], path[MAX_PATH];
    HANDLE mapped, mapping, other;
    DWORD done;
    BYTE *view;
    int i, round;

    GetTempPathW(MAX_PATH, top);
    swprintf(top + wcslen(top), MAX_PATH - wcslen(top), L"written-files-%lu", GetCurrentProcessId());
    if (!CreateDirectoryW(top, NULL)) return fail("mkdir", 0);

    /* one database, a connection per thread, each with its shared lock */
    for (i = 0; i < CONNS; i++)
    {
        if ((conns[i] = open_rw(L"db.sqlite", OPEN_ALWAYS, 0)) == INVALID_HANDLE_VALUE) return fail("open-conn", i);
        if (!lock_shared(conns[i])) return fail("lock-shared", i);
    }
    /* past the first 48, so this handle's descriptor is lent and parked */
    if (!lock_byte(conns[CONNS - 1], LONE_LOCK, FALSE)) return fail("lock-lone", 0);
    if (check_slots(0)) return 1;

    /* files written through their file pointers, in rounds */
    for (i = 0; i < WRITERS + READONLY; i++)
    {
        swprintf(name, 64, L"w%04d.bin", i);
        writers[i] = open_rw(name, CREATE_NEW, i < WRITERS ? 0 : FILE_ATTRIBUTE_READONLY);
        if (writers[i] == INVALID_HANDLE_VALUE) return fail("create-writer", i);
    }
    for (round = 0; round < ROUNDS; round++)
    {
        for (i = 0; i < WRITERS + READONLY; i++)
        {
            fill(data, CHUNK, i, round);
            if (!WriteFile(writers[i], data, CHUNK, &done, NULL) || done != CHUNK) return fail("write-pointer", i);
        }
        if (check_slots(round + 1)) return 1;
    }
    for (i = 0; i < WRITERS + READONLY; i++)
    {
        if (SetFilePointer(writers[i], 0, NULL, FILE_CURRENT) != CHUNK * ROUNDS) return fail("tell", i);
        if (GetFileSize(writers[i], NULL) != CHUNK * ROUNDS) return fail("size", i);
    }
    for (i = 0; i < WRITERS + READONLY; i++)
    {
        if (SetFilePointer(writers[i], 0, NULL, FILE_BEGIN) != 0) return fail("rewind", i);
        if (!ReadFile(writers[i], back, sizeof(back), &done, NULL) || done != sizeof(back)) return fail("read-back", i);
        for (round = 0; round < ROUNDS; round++) fill(data + round * CHUNK, CHUNK, i, round);
        if (memcmp(data, back, sizeof(back))) return fail("writer-data", i);
    }

    /* a file mapped for writing, its handle parked by the traffic around it */
    if ((mapped = open_rw(L"mapped.bin", CREATE_NEW, 0)) == INVALID_HANDLE_VALUE) return fail("create-mapped", 0);
    if (!(mapping = CreateFileMappingW(mapped, NULL, PAGE_READWRITE, 0, 65536, NULL))) return fail("mapping", 0);
    if (!(view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 65536))) return fail("view", 0);
    if (check_slots(ROUNDS + 1)) return 1;
    memset(view + 4096, 0x5a, 512);
    if (!FlushViewOfFile(view, 65536)) return fail("flush-view", 0);
    if (!read_at(mapped, 4096, back, 512)) return fail("read-mapped", 0);
    for (i = 0; i < 512; i++) if (back[i] != 0x5a) return fail("mapped-data", i);
    if (check_slots(ROUNDS + 2)) return 1;
    fill(data, 512, 1, 1);
    if (!write_at(mapped, 8192, data, 512)) return fail("write-mapped", 0);
    if (memcmp(view + 8192, data, 512)) return fail("view-data", 0);

    /* the parked connections' shared locks still hold */
    if ((other = open_rw(L"db.sqlite", OPEN_EXISTING, 0)) == INVALID_HANDLE_VALUE) return fail("open-other", 0);
    if (lock_exclusive(other)) return fail("exclusive-while-shared", 0);
    if (GetLastError() != ERROR_LOCK_VIOLATION) return fail("exclusive-error", 0);
    if (lock_byte(other, LONE_LOCK, TRUE)) return fail("exclusive-while-lone", 0);
    /* closing another handle of the file keeps the parked handle's lock */
    if (!unlock(conns[0])) return fail("unlock", 0);
    CloseHandle(conns[0]);
    conns[0] = NULL;
    if (lock_byte(other, LONE_LOCK, TRUE)) return fail("exclusive-after-close", 0);
    if (!unlock_byte(conns[CONNS - 1], LONE_LOCK)) return fail("unlock-lone", 0);
    if (!lock_byte(other, LONE_LOCK, TRUE)) return fail("exclusive-after-lone", 0);
    if (!unlock_byte(other, LONE_LOCK)) return fail("unlock-lone-exclusive", 0);
    for (i = 1; i < CONNS; i++) if (!unlock(conns[i])) return fail("unlock", i);
    if (!lock_exclusive(other)) return fail("exclusive-after-unlock", 0);
    if (!unlock(other)) return fail("unlock-exclusive", 0);

    if (stress()) return 1;

    UnmapViewOfFile(view);
    CloseHandle(mapping);
    CloseHandle(mapped);
    CloseHandle(other);
    for (i = 1; i < CONNS; i++) CloseHandle(conns[i]);
    for (i = 0; i < WRITERS + READONLY; i++)
    {
        CloseHandle(writers[i]);
        swprintf(name, 64, L"w%04d.bin", i);
        path_of(path, name);
        SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path);
    }
    path_of(path, L"db.sqlite");
    DeleteFileW(path);
    path_of(path, L"mapped.bin");
    DeleteFileW(path);
    RemoveDirectoryW(top);
    printf("written-files verdict=pass connections=%d writers=%d readonly=%d\n", CONNS, WRITERS, READONLY);
    return 0;
}
