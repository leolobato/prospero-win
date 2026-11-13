/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * A commit the host can't give memory to fails as on Windows (wine/patches/1020):
 * with ERROR_COMMITMENT_LIMIT or ERROR_NOT_ENOUGH_MEMORY, the two errors V8 accepts,
 * and the pages stay reserved. A smaller commit in the same range works afterwards,
 * and a failed commit over it leaves that one committed, with its protection and data.
 * Run under a data limit smaller than COMMIT_BYTES (tests/test_wine_commit_limit.py).
 */
#include <windows.h>
#include <stdio.h>

#define RESERVE_BYTES ((SIZE_T)4 << 30)
#define SMALL_BYTES ((SIZE_T)64 << 10)

int main(void)
{
    MEMORY_BASIC_INFORMATION info;
    volatile unsigned char *small;
    unsigned char *base;
    DWORD error, old;

    base = VirtualAlloc(NULL, RESERVE_BYTES, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) {
        printf("commit-limit reserve failed error=%lu verdict=fail\n", GetLastError());
        return 1;
    }
    SetLastError(0xdeadbeef);
    if (VirtualAlloc(base, RESERVE_BYTES, MEM_COMMIT, PAGE_READWRITE)) {
        printf("commit-limit large commit succeeded: no data limit? verdict=fail\n");
        return 1;
    }
    error = GetLastError();
    printf("commit-limit large commit error=%lu\n", error);
    if (error != ERROR_COMMITMENT_LIMIT && error != ERROR_NOT_ENOUGH_MEMORY) {
        printf("commit-limit verdict=fail (V8 aborts on error %lu)\n", error);
        return 1;
    }
    if (!VirtualQuery(base, &info, sizeof(info)) || info.State != MEM_RESERVE ||
        info.RegionSize != RESERVE_BYTES) {
        printf("commit-limit after the failure state=%#lx size=%#llx verdict=fail\n", info.State,
               (unsigned long long)info.RegionSize);
        return 1;
    }
    small = VirtualAlloc(base + SMALL_BYTES, SMALL_BYTES, MEM_COMMIT, PAGE_READWRITE);
    if (!small) {
        printf("commit-limit small commit error=%lu verdict=fail\n", GetLastError());
        return 1;
    }
    small[0] = 0x5a;
    small[SMALL_BYTES - 1] = 0xa5;
    if (small[0] != 0x5a || small[SMALL_BYTES - 1] != 0xa5 || !VirtualQuery(base, &info, sizeof(info)) ||
        info.State != MEM_RESERVE || info.RegionSize != SMALL_BYTES) {
        printf("commit-limit small commit misplaced verdict=fail\n");
        return 1;
    }
    if (!VirtualProtect((void *)small, SMALL_BYTES, PAGE_READONLY, &old)) {
        printf("commit-limit protect error=%lu verdict=fail\n", GetLastError());
        return 1;
    }
    SetLastError(0xdeadbeef);
    if (VirtualAlloc(base, RESERVE_BYTES, MEM_COMMIT, PAGE_READWRITE)) {
        printf("commit-limit mixed commit succeeded verdict=fail\n");
        return 1;
    }
    error = GetLastError();
    printf("commit-limit mixed commit error=%lu\n", error);
    if (error != ERROR_COMMITMENT_LIMIT && error != ERROR_NOT_ENOUGH_MEMORY) {
        printf("commit-limit verdict=fail (V8 aborts on error %lu)\n", error);
        return 1;
    }
    if (!VirtualQuery(base, &info, sizeof(info)) || info.State != MEM_RESERVE || info.RegionSize != SMALL_BYTES ||
        !VirtualQuery((void *)small, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        info.Protect != PAGE_READONLY || info.RegionSize != SMALL_BYTES || small[0] != 0x5a ||
        !VirtualQuery(base + 2 * SMALL_BYTES, &info, sizeof(info)) || info.State != MEM_RESERVE ||
        info.RegionSize != RESERVE_BYTES - 2 * SMALL_BYTES) {
        printf("commit-limit after the mixed failure state=%#lx protect=%#lx size=%#llx verdict=fail\n",
               info.State, info.Protect, (unsigned long long)info.RegionSize);
        return 1;
    }
    VirtualFree(base, 0, MEM_RELEASE);
    printf("commit-limit verdict=pass\n");
    return 0;
}
