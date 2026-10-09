#!/usr/bin/env python3
"""Host test of winebus's PS5 bus (Wine patch 0910): the title's controller
as the HID gamepad DirectInput and winmm find.

Builds tests/lab/pw_hid_pad_sink.c (the title's pad calls, served from a
text file) and tests/lab/pw_hid_pad.c (a PE32 program that sets the pad
through that file and reads it back), then runs the program under a host
Wine built with the patch series (tools/build_host_wine.sh), with the sink
preloaded so that winebus in winedevice finds it. It is not part of
`make test`; it needs the host Wine build and mingw.

Usage: tools/test_wine_hid_pad.py --wine WORK/install/usr/bin/wine [--output DIR]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--wine', required=True, help="the host Wine's wine")
    parser.add_argument('--output', type=Path, help='scratch directory (default: a new temporary one)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = (args.output or Path(tempfile.mkdtemp(prefix='wine-hid-pad-'))).resolve()
    output.mkdir(parents=True, exist_ok=True)

    exe = output / 'pw_hid_pad.exe'
    sink = output / 'pw_hid_pad_sink.so'
    subprocess.run(['i686-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(root / 'tests/lab/pw_hid_pad.c'), '-o', str(exe), '-ldinput8', '-ldxguid', '-lwinmm'],
                   check=True, timeout=60)
    subprocess.run(['gcc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(root / 'tests/lab/pw_hid_pad_sink.c'), '-o', str(sink)], check=True, timeout=60)

    pad = output / 'pad.txt'
    rumble = output / 'rumble.txt'
    pad.write_text('1 0 0 0 0 0 0 0\n')
    rumble.unlink(missing_ok=True)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(output / 'prefix'), WINEDEBUG=env.get('WINEDEBUG', '-all'),
               WINEDLLOVERRIDES='mscoree,mshtml=', LD_PRELOAD=str(sink),
               PW_PAD_FILE=str(pad), PW_RUMBLE_FILE=str(rumble))
    wine = str(Path(args.wine).absolute())
    # Beside wine in an install; in a build tree, server/wineserver.
    wineserver = next((str(p) for p in (Path(wine).with_name('wineserver'), Path(wine).parent / 'server/wineserver')
                       if p.exists()), 'wineserver')
    env['WINESERVER'] = wineserver
    # A fresh server, so that winedevice starts with the sink preloaded.
    subprocess.run([wineserver, '-k'], env=env, check=False, timeout=60)
    subprocess.run([wine, 'wineboot', '-u'], env=env, check=True, timeout=300)
    windows_pad = 'Z:' + str(pad).replace('/', '\\')
    result = subprocess.run([wine, str(exe), windows_pad], env=env, timeout=120)
    subprocess.run([wineserver, '-k'], env=env, check=False, timeout=60)
    if result.returncode:
        raise SystemExit('The HID gamepad checks failed')
    print('The PS5 bus HID gamepad checks passed')


if __name__ == '__main__':
    main()
