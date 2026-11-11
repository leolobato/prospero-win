#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Patch 1017: the server keeps a send buffer size of 0 (Windows' "no send
buffering") without calling setsockopt() on FreeBSD as on macOS, whose
setsockopt() refuses a value below 1.

The case runs in a model of the server's IOCTL_AFD_WINE_SET_SO_SNDBUF, as
FreeBSD and as Linux: 0 is kept without setsockopt() on FreeBSD and passed on
elsewhere; any other value always goes to setsockopt(). On a BSD kernel
(this machine, when it is one) setsockopt(SO_SNDBUF, 0) really fails with
EINVAL, which is what the guard avoids."""
from pathlib import Path
import errno
import os
import shlex
import socket
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/1017-server-accept-a-send-buffer-size-of-0-on-freebsd-too.patch"


def new_side(text, path):
    """The new side of every hunk that changes path, in order."""
    diff = text.split("diff --git a/" + path + " ", 1)[1].split("\ndiff --git ", 1)[0]
    lines = []
    for hunk in diff.split("\n@@")[1:]:
        lines += [x[1:] for x in hunk.splitlines()[1:] if x.startswith((" ", "+")) or x == ""]
    return "\n".join(lines)


source = new_side(PATCH.read_text(), "server/sock.c")
start = source.index("        sndbuf = *(DWORD *)get_req_data();")
end = source.index("        return;\n", source.index("set_error( sock_get_ntstatus( errno ) );", start)) + len("        return;\n")
CASE = source[start:end]
assert "#if defined(__APPLE__) || defined(__FreeBSD__)" in CASE, "the zero guard covers FreeBSD"

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
typedef unsigned int DWORD;
struct sock { DWORD sndbuf; };
static DWORD request = 0;
static int calls, fail_setsockopt, error_set;
#define SOL_SOCKET 1
#define SO_SNDBUF 2
static DWORD *get_req_data(void) { return &request; }
static int setsockopt(int fd, int level, int name, const char *value, int size)
{
    (void)fd; (void)level; (void)name; (void)size;
    calls++;
    if (fail_setsockopt) return -1;
    assert(*(const DWORD *)value == request);
    return 0;
}
static int sock_get_ntstatus(int error) { return error; }
static void set_error(int status) { (void)status; error_set = 1; }
static int errno_value;
#define errno errno_value

static void set_sndbuf(struct sock *sock, int unix_fd)
{
        DWORD sndbuf;
/*CASE*/
}

int main(void)
{
    struct sock sock = { 1234 };

    request = 0; calls = 0; error_set = 0; fail_setsockopt = ZERO_FAILS;
    set_sndbuf(&sock, 3);
    assert(sock.sndbuf == 0 && !error_set);
    assert(calls == (ZERO_KEPT ? 0 : 1));

    request = 65536; calls = 0; error_set = 0; fail_setsockopt = 0;
    set_sndbuf(&sock, 3);
    assert(sock.sndbuf == 65536 && calls == 1 && !error_set);

    request = 1; calls = 0; error_set = 0; fail_setsockopt = 1;
    set_sndbuf(&sock, 3);
    assert(sock.sndbuf == 65536 && calls == 1 && error_set);
    puts("send buffer passed");
    return 0;
}
'''


def run(defines, zero_kept):
    # The platform the case is compiled for, without defining __FreeBSD__ or __APPLE__ for this machine's headers.
    case = CASE.replace("defined(__APPLE__)", "defined(MODEL_APPLE)").replace("defined(__FreeBSD__)", "defined(MODEL_FREEBSD)")
    assert "__APPLE__" not in case and "__FreeBSD__" not in case
    code = HARNESS.replace("/*CASE*/", case)
    with tempfile.TemporaryDirectory() as directory:
        source_path, exe = Path(directory) / "sndbuf.c", Path(directory) / "sndbuf"
        source_path.write_text(code)
        command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", "-O2"))
        command += ["-std=gnu11", "-Wall", "-Wno-unused-function"] + defines
        command += [f"-DZERO_KEPT={int(zero_kept)}", f"-DZERO_FAILS={int(zero_kept)}"]
        subprocess.run(command + [str(source_path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


run(["-DMODEL_FREEBSD"], True)
run(["-DMODEL_APPLE"], True)
run([], False)

if sys.platform.startswith(("darwin", "freebsd")):
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 0)
        raise AssertionError("a BSD kernel took SO_SNDBUF = 0")
    except OSError as error:
        assert error.errno == errno.EINVAL, error
    finally:
        probe.close()
    print("this BSD kernel refuses SO_SNDBUF = 0 with EINVAL")
