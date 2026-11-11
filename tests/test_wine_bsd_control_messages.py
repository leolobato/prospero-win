#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run patch 1016's control message cases (IP_RECVTOS and IP_RECVTTL, the
labels FreeBSD and macOS give the received TOS and TTL) in a model of
convert_control_headers' IPPROTO_IP switch: each becomes WS_IP_TOS or
WS_IP_TTL with its single byte as an INT, and nothing past that byte is read.

Then this machine's kernel: a UDP datagram sent over loopback with a TOS, to
a socket that asked for IP_RECVTOS and IP_RECVTTL, comes back with control
messages the switch converts. On the BSDs they are labelled IP_RECVTOS and
IP_RECVTTL, the patch's cases; on Linux IP_TOS and IP_TTL, Wine's own."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/1016-ntdll-convert-the-received-tos-and-ttl-control-messages-the-bsds-send.patch"


def new_side(text, path):
    """The new side of every hunk that changes path, in order."""
    diff = text.split("diff --git a/" + path + " ", 1)[1].split("\ndiff --git ", 1)[0]
    lines = []
    for hunk in diff.split("\n@@")[1:]:
        lines += [x[1:] for x in hunk.splitlines()[1:] if x.startswith((" ", "+")) or x == ""]
    return "\n".join(lines)


def block(source, start, end):
    first = source.index(start)
    last = source.index(end, first) + len(end)
    return source[first:last]


source = new_side(PATCH.read_text(), "dlls/ntdll/unix/socket.c")
RECVTOS = block(source, "#if defined(IP_RECVTOS)", "#endif /* IP_RECVTOS */")
RECVTTL = block(source, "#if defined(IP_RECVTTL)", "#endif /* IP_RECVTTL */")
assert "case IP_RECVTOS:" in RECVTOS and "WS_IP_TOS" in RECVTOS
assert "case IP_RECVTTL:" in RECVTTL and "WS_IP_TTL" in RECVTTL

HARNESS = r'''
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef int INT;
typedef unsigned int ULONG;
#define WS_IPPROTO_IP 0
#define WS_IP_TOS 3
#define WS_IP_TTL 4

static int got_level = -1, got_type = -1, got_value = -1, unhandled;

static char *fill_control_message( int level, int type, char *current, ULONG *maxsize, void *data, int len )
{
    assert(len == sizeof(INT));
    got_level = level; got_type = type;
    memcpy(&got_value, data, sizeof(INT));
    (void)maxsize;
    return current + 1;
}

/* Wine's IPPROTO_IP switch, with the patch's cases in place. */
static int convert( struct cmsghdr *cmsg_unix )
{
    char buffer[16], *ptr = buffer;
    ULONG ctlsize = sizeof(buffer);

    got_level = got_type = got_value = -1;
    switch (cmsg_unix->cmsg_type)
    {
                    case IP_TOS:
                    {
                        INT tos = *(unsigned char *)CMSG_DATA(cmsg_unix);
                        ptr = fill_control_message( WS_IPPROTO_IP, WS_IP_TOS, ptr, &ctlsize,
                                                    &tos, sizeof(INT) );
                        if (!ptr) goto error;
                        break;
                    }

/*RECVTOS*/

                    case IP_TTL:
                    {
                        ptr = fill_control_message( WS_IPPROTO_IP, WS_IP_TTL, ptr, &ctlsize,
                                                    CMSG_DATA(cmsg_unix), sizeof(INT) );
                        if (!ptr) goto error;
                        break;
                    }

/*RECVTTL*/

                    default:
                        unhandled++;
                        break;
    }
    return 1;
error:
    return 0;
}

static void one_byte( int type, unsigned char value, int want_type )
{
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } message;

    memset(&message, 0xff, sizeof(message)); /* the bytes past the one sent are garbage */
    message.header.cmsg_level = IPPROTO_IP;
    message.header.cmsg_type = type;
    message.header.cmsg_len = CMSG_LEN(1);
    *CMSG_DATA(&message.header) = value;
    assert(convert(&message.header));
    assert(got_level == WS_IPPROTO_IP && got_type == want_type && got_value == value);
}

int main(void)
{
    int receiver, sender, on = 1, tos = 0x20, seen_tos = 0, seen_ttl = 0;
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    char payload[4] = "tos", data[16];
    union { struct cmsghdr header; char bytes[256]; } control;
    struct iovec io = { data, sizeof(data) };
    struct msghdr msg;
    struct cmsghdr *cmsg;

    /* The patch's cases: one byte each, as the BSDs send them. */
    one_byte(IP_RECVTOS, 0xb8, WS_IP_TOS);
    one_byte(IP_RECVTOS, 0x00, WS_IP_TOS);
    one_byte(IP_RECVTTL, 64, WS_IP_TTL);
    one_byte(IP_RECVTTL, 255, WS_IP_TTL);
    assert(!unhandled);

    /* This kernel's labels. */
    receiver = socket(AF_INET, SOCK_DGRAM, 0);
    sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!bind(receiver, (struct sockaddr *)&address, sizeof(address)));
    assert(!getsockname(receiver, (struct sockaddr *)&address, &length));
    assert(!setsockopt(receiver, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on)));
    assert(!setsockopt(receiver, IPPROTO_IP, IP_RECVTTL, &on, sizeof(on)));
    assert(!setsockopt(sender, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)));
    assert(sendto(sender, payload, sizeof(payload), 0, (struct sockaddr *)&address, sizeof(address)) == sizeof(payload));
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
    assert(recvmsg(receiver, &msg, 0) == sizeof(payload));
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        if (cmsg->cmsg_level != IPPROTO_IP) continue;
        printf("kernel label %d (IP_TOS %d, IP_RECVTOS %d, IP_TTL %d, IP_RECVTTL %d), %u data bytes\n",
               cmsg->cmsg_type, IP_TOS, IP_RECVTOS, IP_TTL, IP_RECVTTL,
               (unsigned)(cmsg->cmsg_len - CMSG_LEN(0)));
        assert(convert(cmsg));
        if (got_type == WS_IP_TOS) { assert(got_value == tos); seen_tos = 1; }
        if (got_type == WS_IP_TTL) { assert(got_value > 0 && got_value <= 255); seen_ttl = 1; }
    }
    assert(!unhandled && seen_tos && seen_ttl);
    close(receiver);
    close(sender);
    puts("control messages passed: BSD and Linux labels of the received TOS and TTL convert");
    return 0;
}
'''


def run():
    code = HARNESS.replace("/*RECVTOS*/", RECVTOS).replace("/*RECVTTL*/", RECVTTL)
    with tempfile.TemporaryDirectory() as directory:
        source_path, exe = Path(directory) / "control_messages.c", Path(directory) / "control_messages"
        source_path.write_text(code)
        command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", "-O2"))
        command += ["-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function"]
        subprocess.run(command + [str(source_path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


run()
