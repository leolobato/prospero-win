#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run patch 1018's dns_info_alloc (whole in the patch's hunk) against a model
of dnsapi's DnsQueryConfig and the process heap.

- No DNS configuration (dnsapi without its Unix side, as on a PS5: every call
  fails with ERROR_NOT_SUPPORTED): GetAdaptersAddresses' DNS step succeeds,
  every adapter is kept with no DNS servers and an empty suffix.
- A machine with DNS: the servers (after an ERROR_MORE_DATA retry when they
  don't fit the stack buffer) and the search suffix are filled in as before.
- Running out of memory, in DnsQueryConfig or for a server entry, still fails
  the call."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/1018-iphlpapi-leave-the-dns-servers-out-when-dnsqueryconfig-fails.patch"


def new_side(text, path):
    """The new side of every hunk that changes path, in order."""
    diff = text.split("diff --git a/" + path + " ", 1)[1].split("\ndiff --git ", 1)[0]
    lines = []
    for hunk in diff.split("\n@@")[1:]:
        lines += [x[1:] for x in hunk.splitlines()[1:] if x.startswith((" ", "+")) or x == ""]
    return "\n".join(lines)


def function(source, signature):
    """The definition that starts with signature, through its closing brace."""
    match = re.search(r"^" + re.escape(signature), source, re.M)
    assert match, signature
    end = source.index("\n{", match.start()) + 2
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


CODE = function(new_side(PATCH.read_text(), "dlls/iphlpapi/iphlpapi_main.c"), "static DWORD dns_info_alloc(")
assert "if (err == ERROR_NOT_ENOUGH_MEMORY) return err;" in CODE

HARNESS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int DWORD, ULONG;
typedef unsigned short WORD, WCHAR;
typedef int INT;
typedef char CHAR;
#define ERROR_SUCCESS 0
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NOT_SUPPORTED 50
#define ERROR_MORE_DATA 234
#define CP_ACP 0
#define HEAP_ZERO_MEMORY 8
#define MAX_ADAPTER_NAME_LENGTH 256
#define MAX_DNS_SUFFIX_STRING_LENGTH 256
#define GAA_FLAG_SKIP_DNS_SERVER 0x8
#define FIELD_OFFSET(type, field) ((int)__builtin_offsetof(type, field))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
enum { DnsConfigSearchList = 100, QUERY_SERVERS = 200 };

typedef struct { WORD sa_family; char sa_data[14]; } SOCKADDR;
typedef struct { CHAR MaxSa[32]; union { DWORD DnsAddrUserDword[8]; } Data; } DNS_ADDR;
typedef struct
{
    DWORD MaxCount, AddrCount, Tag;
    WORD Family, WordReserved;
    DWORD Flags, MatchFlag, Reserved1, Reserved2;
    DNS_ADDR AddrArray[1];
} DNS_ADDR_ARRAY;
typedef struct _IP_ADAPTER_DNS_SERVER_ADDRESS
{
    ULONG Length;
    struct _IP_ADAPTER_DNS_SERVER_ADDRESS *Next;
    struct { SOCKADDR *lpSockaddr; INT iSockaddrLength; } Address;
} IP_ADAPTER_DNS_SERVER_ADDRESS;
typedef struct _IP_ADAPTER_ADDRESSES
{
    struct _IP_ADAPTER_ADDRESSES *Next;
    char *AdapterName;
    IP_ADAPTER_DNS_SERVER_ADDRESS *FirstDnsServerAddress;
    WCHAR *DnsSuffix;
} IP_ADAPTER_ADDRESSES;

static size_t wlen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static WCHAR *wcopy(WCHAR *d, const WCHAR *s) { size_t i = 0; do d[i] = s[i]; while (s[i++]); return d; }
#define wcslen wlen
#define wcscpy wcopy

static int MultiByteToWideChar(int cp, DWORD flags, const char *src, int n, WCHAR *dst, int cap)
{
    int i = 0;
    assert(cp == CP_ACP && !flags && n == -1);
    for (; src[i] && i + 1 < cap; i++) dst[i] = (unsigned char)src[i];
    dst[i] = 0;
    return i + 1;
}

/* The heap: counts what is left and can fail on request. */
static int heap_live, heap_fail_at = -1, heap_calls;
static void *GetProcessHeap(void) { return NULL; }
static void *HeapAlloc(void *heap, DWORD flags, size_t size)
{
    (void)heap;
    if (heap_calls++ == heap_fail_at) return NULL;
    heap_live++;
    return flags & HEAP_ZERO_MEMORY ? calloc(1, size) : malloc(size);
}

/* DnsQueryConfig: no configuration (failure), or n servers and a suffix. */
static DWORD dns_failure = ERROR_NOT_SUPPORTED;
static int servers_wanted, dns_calls;
static const char *suffix = "";
static DWORD dns_servers_query_code(ULONG family) { (void)family; return QUERY_SERVERS; }
static DWORD DnsQueryConfig(int type, DWORD flag, const WCHAR *adapter, void *reserved, void *buffer, DWORD *len)
{
    (void)flag; (void)reserved;
    assert(adapter && adapter[0]);
    dns_calls++;
    if (dns_failure) return dns_failure;
    if (type == DnsConfigSearchList)
    {
        DWORD need = (DWORD)((strlen(suffix) + 1) * sizeof(WCHAR));
        if (!buffer) { *len = need; return ERROR_SUCCESS; }
        for (size_t i = 0; i <= strlen(suffix); i++) ((WCHAR *)buffer)[i] = (unsigned char)suffix[i];
        return ERROR_SUCCESS;
    }
    assert(type == QUERY_SERVERS);
    {
        DWORD need = FIELD_OFFSET(DNS_ADDR_ARRAY, AddrArray[servers_wanted]);
        DNS_ADDR_ARRAY *array = buffer;
        /* The stack buffer holds three; more come after ERROR_MORE_DATA with a heap buffer of *len. */
        if (servers_wanted > 3 && dns_calls % 2 == 1) { *len = need; return ERROR_MORE_DATA; }
        array->AddrCount = servers_wanted;
        for (int i = 0; i < servers_wanted; i++)
        {
            memset(&array->AddrArray[i], 0, sizeof(DNS_ADDR));
            array->AddrArray[i].MaxSa[0] = 2; /* AF_INET */
            array->AddrArray[i].MaxSa[4] = 10 + i;
            array->AddrArray[i].Data.DnsAddrUserDword[0] = 16;
        }
        return ERROR_SUCCESS;
    }
}

/*CODE*/

static IP_ADAPTER_ADDRESSES adapters[2];

static void reset(void)
{
    memset(adapters, 0, sizeof(adapters));
    adapters[0].AdapterName = "{0001}"; adapters[0].Next = &adapters[1];
    adapters[1].AdapterName = "{0002}";
    heap_live = heap_calls = dns_calls = 0; heap_fail_at = -1;
}

static int servers_of(const IP_ADAPTER_ADDRESSES *aa)
{
    int n = 0;
    for (const IP_ADAPTER_DNS_SERVER_ADDRESS *s = aa->FirstDnsServerAddress; s; s = s->Next)
    {
        assert(s->Length == sizeof(*s) && s->Address.iSockaddrLength == 16);
        assert((unsigned char)((char *)s->Address.lpSockaddr)[4] == 10 + n);
        n++;
    }
    return n;
}

int main(void)
{
    /* A PS5: no DNS configuration. */
    reset(); dns_failure = ERROR_NOT_SUPPORTED;
    assert(dns_info_alloc(adapters, 0, 0) == ERROR_SUCCESS);
    for (int i = 0; i < 2; i++)
        assert(!adapters[i].FirstDnsServerAddress && adapters[i].DnsSuffix && !adapters[i].DnsSuffix[0]);
    assert(dns_calls == 4); /* servers and search list, per adapter */

    /* A machine with DNS: two servers in the stack buffer, five through ERROR_MORE_DATA. */
    dns_failure = ERROR_SUCCESS; suffix = "lan";
    for (int wanted = 2; wanted <= 5; wanted += 3)
    {
        reset(); servers_wanted = wanted;
        assert(dns_info_alloc(adapters, 0, 0) == ERROR_SUCCESS);
        for (int i = 0; i < 2; i++)
        {
            assert(servers_of(&adapters[i]) == wanted);
            assert(wlen(adapters[i].DnsSuffix) == 3 && adapters[i].DnsSuffix[0] == 'l');
        }
    }
    /* GAA_FLAG_SKIP_DNS_SERVER: no server query at all. */
    reset(); servers_wanted = 2;
    assert(dns_info_alloc(adapters, 0, GAA_FLAG_SKIP_DNS_SERVER) == ERROR_SUCCESS);
    assert(!adapters[0].FirstDnsServerAddress && dns_calls == 4);

    /* Out of memory still fails: in DnsQueryConfig, and for a server entry. */
    reset(); dns_failure = ERROR_NOT_ENOUGH_MEMORY;
    assert(dns_info_alloc(adapters, 0, 0) == ERROR_NOT_ENOUGH_MEMORY);
    reset(); dns_failure = ERROR_SUCCESS; servers_wanted = 2; heap_fail_at = 1;
    assert(dns_info_alloc(adapters, 0, 0) == ERROR_NOT_ENOUGH_MEMORY);
    puts("adapters without DNS passed: no DNS configuration keeps the adapters, memory failures still fail");
    return 0;
}
'''


def run():
    code = HARNESS.replace("/*CODE*/", CODE)
    with tempfile.TemporaryDirectory() as directory:
        source_path, exe = Path(directory) / "adapters_dns.c", Path(directory) / "adapters_dns"
        source_path.write_text(code)
        command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", "-O2"))
        command += ["-std=gnu11", "-Wall", "-Wno-unused-function", "-Wno-unused-variable"]
        subprocess.run(command + [str(source_path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


run()
