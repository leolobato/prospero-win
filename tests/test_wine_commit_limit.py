#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A commit the host can't give memory to fails with the commit limit and
leaves the pages reserved, as on Windows (wine/patches/1020). Wine runs under
a 1 GiB data limit (RLIMIT_DATA, which Linux checks when mprotect() makes
private memory writable) and the program commits 4 GiB it reserved: without
the patch the error is ERROR_ACCESS_DENIED, which V8 treats as a fatal bug.

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and x86_64-w64-mingw32-gcc; without
either the check is skipped with a message."""
from __future__ import annotations

import os
import resource
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_commit_limit.c"
DATA_LIMIT = 1 << 30


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine commit limit skipped: needs the host Wine and x86_64-w64-mingw32-gcc")
        return 0

    def limit_data():
        resource.setrlimit(resource.RLIMIT_DATA, (DATA_LIMIT, DATA_LIMIT))

    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "wine_commit_limit.exe"
        subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE)], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
        # The prefix is made without the limit: wineboot's own memory is not the test.
        subprocess.run([wine, "wineboot", "-i"], capture_output=True, env=env, check=False)
        subprocess.run([str(Path(wine).with_name("wineserver")), "-w"], env=env, check=False)
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env,
                              preexec_fn=limit_data)
        subprocess.run([str(Path(wine).with_name("wineserver")), "-k"], env=env, check=False)
        assert done.returncode == 0 and "commit-limit verdict=pass" in done.stdout, f"{done.stdout}{done.stderr}"
    print("wine commit limit passed: a commit past the data limit fails with the commit limit,"
          " the pages stay reserved, a smaller commit works and keeps its protection after another failure")
    return 0


if __name__ == "__main__":
    sys.exit(main())
