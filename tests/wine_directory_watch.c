/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Many directories can be watched at once (patch 1019). Java's watch
 * service, which Project Zomboid uses on every folder of its media tree,
 * opens each folder for overlapped I/O, reads its file information, binds it
 * to one completion port and keeps a ReadDirectoryChangesW pending on it. On
 * the console every such handle held a file the title can have only about
 * 256 of. This watches 1500 folders that way, then creates a file in three
 * of them and renames one in a fourth: each change must reach the port with
 * the folder's key and the right names, the watch is queued again as Java
 * does, and no other folder may report anything. Last, a regular file must
 * still open, so the watches left room for the title's other files. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define COUNT 1500
#define BUFFER 1024
#define FILTER (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE)
#define WAIT_MS 60000
#define CREATED (int)(sizeof(created) / sizeof(created[0]))

static HANDLE dirs[COUNT];
static OVERLAPPED overlapped[COUNT];
static DWORD buffers[COUNT][BUFFER / sizeof(DWORD)];
static WCHAR top[MAX_PATH];

static int fail(const char *what, int index)
{
    printf("directory-watch failed=%s index=%d error=%lu\n", what, index, GetLastError());
    return 1;
}

static void path_in(WCHAR *out, int dir, const WCHAR *name)
{
    if (name) swprintf(out, MAX_PATH, L"%ls\\d%d\\%ls", top, dir, name);
    else swprintf(out, MAX_PATH, L"%ls\\d%d", top, dir);
}

static BOOL create_file(int dir, const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD done;

    path_in(path, dir, name);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    WriteFile(file, "x", 1, &done, NULL);
    CloseHandle(file);
    return TRUE;
}

static BOOL watch(int i)
{
    memset(&overlapped[i], 0, sizeof(overlapped[i]));
    return ReadDirectoryChangesW(dirs[i], buffers[i], BUFFER, FALSE, FILTER, NULL, &overlapped[i], NULL);
}

/* the actions reported for one folder, as "<action>:<name>;" entries */
static void describe(int i, DWORD size, char *out, size_t out_size)
{
    FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)buffers[i];
    size_t used = 0;

    out[0] = 0;
    if (!size) return;
    for (;;)
    {
        char name[MAX_PATH];
        int len = WideCharToMultiByte(CP_UTF8, 0, info->FileName, info->FileNameLength / sizeof(WCHAR),
                                      name, sizeof(name) - 1, NULL, NULL);
        name[len > 0 ? len : 0] = 0;
        used += snprintf(out + used, out_size - used, "%lu:%s;", info->Action, name);
        if (!info->NextEntryOffset || used >= out_size) break;
        info = (FILE_NOTIFY_INFORMATION *)((BYTE *)info + info->NextEntryOffset);
    }
}

int main(void)
{
    static const int created[] = { 0, 700, COUNT - 1 };
    static const int renamed = 1000;
    char seen[COUNT][256];
    WCHAR path[MAX_PATH], to[MAX_PATH];
    BY_HANDLE_FILE_INFORMATION info;
    ULONGLONG start;
    HANDLE port, file;
    int i, pending, reports = 0;

    GetTempPathW(MAX_PATH, path);
    swprintf(top, MAX_PATH, L"%lsdirectory-watch-%lu-%llu", path, GetCurrentProcessId(), GetTickCount64());
    if (!CreateDirectoryW(top, NULL)) return fail("mkdir", 0);
    for (i = 0; i < COUNT; i++)
    {
        path_in(path, i, NULL);
        if (!CreateDirectoryW(path, NULL)) return fail("mkdir", i);
        if (!create_file(i, L"x.txt")) return fail("create", i);
    }

    if (!(port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0))) return fail("port", 0);
    for (i = 0; i < COUNT; i++)
    {
        path_in(path, i, NULL);
        dirs[i] = CreateFileW(path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        if (dirs[i] == INVALID_HANDLE_VALUE) return fail("open", i);
        if (!GetFileInformationByHandle(dirs[i], &info)) return fail("information", i);
        if (CreateIoCompletionPort(dirs[i], port, i, 0) != port) return fail("bind", i);
        if (!watch(i)) return fail("watch", i);
    }

    /* the watches must leave room for other files */
    path_in(path, 0, L"..\\room.bin");
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) return fail("room", 0);
    CloseHandle(file);

    /* let the first listings settle before anything changes */
    Sleep(500);
    memset(seen, 0, sizeof(seen));
    start = GetTickCount64();
    for (i = 0; i < CREATED; i++)
        if (!create_file(created[i], L"new.txt")) return fail("create-new", created[i]);
    path_in(path, renamed, L"x.txt");
    path_in(to, renamed, L"y.txt");
    if (!MoveFileW(path, to)) return fail("rename", renamed);

    /* every expected change, and nothing else */
    for (pending = CREATED + 1; pending > 0;)
    {
        DWORD size;
        ULONG_PTR key;
        OVERLAPPED *done;
        size_t len;

        ULONGLONG now = GetTickCount64();
        BOOL ok = GetQueuedCompletionStatus(port, &size, &key, &done,
                                            now < start + WAIT_MS ? (DWORD)(start + WAIT_MS - now) : 0);

        if (!ok && !done)
        {
            for (i = 0; i < CREATED; i++)
                if (!strstr(seen[created[i]], "1:new.txt;")) printf("directory-watch missing=d%d\n", created[i]);
            if (!strstr(seen[renamed], "4:x.txt;") || !strstr(seen[renamed], "5:y.txt;"))
                printf("directory-watch missing=d%d seen=%s\n", renamed, seen[renamed]);
            return fail("timeout", pending);
        }
        if (key >= COUNT || done != &overlapped[key]) return fail("key", (int)key);
        if (!ok) return fail("completion", (int)key);
        reports++;
        len = strlen(seen[key]);
        describe((int)key, size, seen[key] + len, sizeof(seen[key]) - len);
        if (!watch((int)key)) return fail("watch-again", (int)key);

        pending = 0;
        for (i = 0; i < CREATED; i++)
            if (!strstr(seen[created[i]], "1:new.txt;")) pending++;
        if (!strstr(seen[renamed], "4:x.txt;") || !strstr(seen[renamed], "5:y.txt;")) pending++;
    }
    printf("directory-watch reported in %llu ms, %d completions\n", GetTickCount64() - start, reports);
    for (i = 0; i < COUNT; i++)
    {
        int expected = i == renamed || i == created[0] || i == created[1] || i == created[2];
        if (!expected && seen[i][0])
        {
            printf("directory-watch unexpected=d%d seen=%s\n", i, seen[i]);
            return fail("unexpected", i);
        }
    }

    for (i = 0; i < COUNT; i++) CloseHandle(dirs[i]);
    CloseHandle(port);
    for (i = 0; i < COUNT; i++)
    {
        path_in(path, i, L"x.txt");
        DeleteFileW(path);
        path_in(path, i, L"y.txt");
        DeleteFileW(path);
        path_in(path, i, L"new.txt");
        DeleteFileW(path);
        path_in(path, i, NULL);
        RemoveDirectoryW(path);
    }
    RemoveDirectoryW(top);
    printf("directory-watch verdict=pass directories=%d\n", COUNT);
    return 0;
}
