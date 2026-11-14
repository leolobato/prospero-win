#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Many files can be kept open for writing, and locked, at once
(wine/patches/1021): one database opened 300 times with a shared lock through
each handle, as SQLite in WAL mode does with a connection per thread, 400 files
written through their file pointers in rounds, 40 created read-only, and a file
mapped for writing; then an exclusive lock must still conflict with the parked
handles' shared locks, and eight threads append records through their file
pointers while two others keep opening files.

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and x86_64-w64-mingw32-gcc; without either
the check is skipped with a message. The PC passes without the patch too: the
same program is the console's check. PROSPERO_TEST_NOFILE=<n> runs Wine and its
server under that descriptor limit; with 512, a server built with only 0993
(-DPS5_PARK_DIRECTORIES -DPS5_PARK_FILES) fails with
STATUS_TOO_MANY_OPENED_FILES at the 244th connection, and a server and ntdll
with this patch, both built with those flags (the loans need both), pass. Under a
limit the program also runs three times with budgets of 4 files
(WINE_PS5_PARK_FILES, WINE_PS5_CACHED_FILES), where the writers' files are
parked and opened again all the time."""
from __future__ import annotations

import os
import resource
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_written_files.c"


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine written files skipped: needs the host Wine and x86_64-w64-mingw32-gcc")
        return 0
    limit = int(os.environ.get("PROSPERO_TEST_NOFILE", "0"))

    def lower_limit():
        if limit:
            resource.setrlimit(resource.RLIMIT_NOFILE, (limit, limit))

    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "wine_written_files.exe"
        subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE), "-lntdll"], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
        # under a limit, also with budgets of 4 files, so that files are parked
        # and opened again all the time while the threads write
        budgets = [{}] + ([{"WINE_PS5_PARK_FILES": "4", "WINE_PS5_CACHED_FILES": "4"}] * 3 if limit else [])
        for budget in budgets:
            # the server reads the budgets when it starts
            server = os.environ.get("WINESERVER", str(Path(wine).parent / "wineserver"))
            subprocess.run([server, "-k"], env=env, capture_output=True)
            subprocess.run([server, "-w"], env=env, capture_output=True)
            done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=dict(env, **budget),
                                  preexec_fn=lower_limit)
            assert done.returncode == 0, f"{budget}: {done.stdout}{done.stderr}"
            assert "written-files verdict=pass" in done.stdout, f"{budget}: {done.stdout}"
    print("wine written files passed: one database open 300 times with shared locks, 400 files written"
          " through their pointers, 40 read-only, a mapped file, eight writers against two openers"
          + (f" (descriptor limit {limit}, and three runs with budgets of 4 files)" if limit else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
