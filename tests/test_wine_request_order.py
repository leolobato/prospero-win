#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A Wine patch that adds a server request adds it after every other one.

A request's number is its place in server/protocol.def. One added in the
middle renumbers every request after it, and a PE module that a patch doesn't
change, so the runtime keeps the earlier build (ntoskrnl.exe, conhost.exe),
then sends the wrong requests. Patch 1021 first added release_fd_loan after
get_handle_fd: ntoskrnl.exe's get_kernel_object_ptr became another request,
KeGetCurrentThread returned NULL, and every game that hosts the controller's
HID crashed at start.

Each patch's hunks of server/protocol.def and of the generated enum request
in include/wine/server_protocol.h are read: an added request must have no
earlier request after it."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / "wine" / "patches"


def hunks(text: str, path: str) -> list[list[str]]:
    """The hunks of one file in a patch, each a list of its body lines."""
    found: list[list[str]] = []
    current = None
    in_file = False
    for line in text.splitlines():
        if line.startswith("diff --git "):
            in_file = line.endswith(" b/" + path)
            current = None
            continue
        if not in_file:
            continue
        if line.startswith("@@"):
            current = []
            found.append(current)
        elif current is not None and line[:1] in (" ", "+", "-", ""):
            current.append(line)
    return found


def problems(name: str, text: str) -> list[str]:
    found = []
    for hunk in hunks(text, "server/protocol.def"):
        added = [i for i, line in enumerate(hunk) if line.startswith("+@REQ(")]
        if added and any(not line.startswith("+") for line in hunk[added[-1]:]):
            req = hunk[added[-1]][1:].strip()
            found.append(f"{name}: {req} is added before other lines of server/protocol.def")
    for hunk in hunks(text, "include/wine/server_protocol.h"):
        req_line = re.compile(r"^[ +-]    REQ_\w+,$")
        for i, line in enumerate(hunk):
            if not (line.startswith("+    REQ_") and req_line.match(line)):
                continue
            later = [later for later in hunk[i + 1:] if req_line.match(later) and not later.startswith("+")]
            if later:
                found.append(f"{name}: {line[1:].strip()} comes before {later[0][1:].strip()} in enum request")
    return found


def main() -> int:
    patches = sorted(PATCHES.glob("*.patch"))
    if not patches:
        print("wine request order: no patches", file=sys.stderr)
        return 1
    found = [p for patch in patches for p in problems(patch.name, patch.read_text(errors="replace"))]
    for problem in found:
        print(problem, file=sys.stderr)
    if found:
        print("wine request order: add new server requests at the end of server/protocol.def", file=sys.stderr)
        return 1
    print(f"wine request order: {len(patches)} patches add requests only at the end")
    return 0


if __name__ == "__main__":
    sys.exit(main())
