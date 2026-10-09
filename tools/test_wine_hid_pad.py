#!/usr/bin/env python3
"""Host test of the PS5 title's controller as a HID gamepad (Wine patches
0910-0912), as DirectInput and winmm find it.

Builds tests/lab/pw_hid_pad_sink.c (the title's pad calls, served from a
text file) and tests/lab/pw_hid_pad.c (a program that sets the pad through
that file and reads it back), then runs the program under a host Wine built
with the patch series (tools/build_host_wine.sh), with the sink preloaded.
By default winebus runs in winedevice. With --hosted, a PE32 and a PE64
program each host the HID drivers themselves, with services.exe disabled
and the gamepad never installed before, as in a PS5 title.

It is not part of `make test`: it needs the host Wine build and mingw, on
x86_64 Linux (Rosetta's signal handling asserts in Wine).

Usage: tools/test_wine_hid_pad.py --wine WORK/install/usr/bin/wine [--hosted] [--output DIR]
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
    parser.add_argument('--hosted', action='store_true',
                        help='run the HID drivers in the test program (WINE_NTOSKRNL_HOSTED), 32- and 64-bit, '
                             'as a PS5 title does: without services.exe or winedevice')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = (args.output or Path(tempfile.mkdtemp(prefix='wine-hid-pad-'))).resolve()
    output.mkdir(parents=True, exist_ok=True)

    programs = {'pe32': ('i686-w64-mingw32-gcc', output / 'pw_hid_pad.exe')}
    if args.hosted:
        programs['pe64'] = ('x86_64-w64-mingw32-gcc', output / 'pw_hid_pad64.exe')
    for compiler, exe in programs.values():
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(root / 'tests/lab/pw_hid_pad.c'), '-o', str(exe), '-ldinput8', '-ldxguid', '-lwinmm', '-lruntimeobject'],
                       check=True, timeout=60)
    sink = output / 'pw_hid_pad_sink.so'
    subprocess.run(['gcc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(root / 'tests/lab/pw_hid_pad_sink.c'), '-o', str(sink)], check=True, timeout=60)

    pad = output / 'pad.txt'
    rumble = output / 'rumble.txt'
    pad.write_text('1 0 0 0 0 0 0 0\n')
    rumble.unlink(missing_ok=True)
    wine = str(Path(args.wine).absolute())
    # Beside wine in an install; in a build tree, server/wineserver.
    wineserver = next((str(p) for p in (Path(wine).with_name('wineserver'), Path(wine).parent / 'server/wineserver')
                       if p.exists()), 'wineserver')
    plain = os.environ.copy()
    plain.update(WINEPREFIX=str(output / 'prefix'), WINEDEBUG=plain.get('WINEDEBUG', '-all'),
                 WINEDLLOVERRIDES='mscoree,mshtml=', WINESERVER=wineserver)
    with_pad = dict(plain, LD_PRELOAD=str(sink), PW_PAD_FILE=str(pad), PW_RUMBLE_FILE=str(rumble))
    windows_pad = 'Z:' + str(pad).replace('/', '\\')
    windows_rumble = 'Z:' + str(rumble).replace('/', '\\')

    subprocess.run([wineserver, '-k'], env=plain, check=False, timeout=60)
    failed = []
    if not args.hosted:
        # A fresh server whose winedevice has the pad.
        subprocess.run([wine, 'wineboot', '-u'], env=with_pad, check=True, timeout=300)
        if subprocess.run([wine, str(programs['pe32'][1]), windows_pad, windows_rumble], env=with_pad, timeout=120).returncode:
            failed.append('winedevice')
    else:
        # As on the console: no services.exe (so no service manager and no winedevice), and the
        # gamepad never installed before. Only the test program, with WINE_NTOSKRNL_HOSTED, can
        # install and make it; each program starts on a fresh server.
        subprocess.run([wine, 'wineboot', '-u'], env=plain, check=True, timeout=300)
        hosted = dict(with_pad, WINE_NTOSKRNL_HOSTED='1', WINEDEBUG='err+all,trace+ntoskrnl',
                      WINEDLLOVERRIDES='services.exe=d;mscoree,mshtml=')
        for name, (_, exe) in programs.items():
            subprocess.run([wineserver, '-k'], env=plain, check=False, timeout=60)
            log = output / f'hosted-{name}.log'
            with open(log, 'w') as out:
                code = subprocess.run([wine, str(exe), windows_pad, windows_rumble], env=hosted, stdout=out, stderr=subprocess.STDOUT,
                                      timeout=120).returncode
            text = log.read_text(errors='replace')
            for line in text.splitlines():
                if line.startswith(('ok ', 'FAIL ')) or 'started, ' in line or 'hosted drivers failed' in line:
                    print(f'{name}: {line}')
            if code or 'started, gamepad after' not in text:
                failed.append(name)
    subprocess.run([wineserver, '-k'], env=plain, check=False, timeout=60)
    if failed:
        raise SystemExit('The HID gamepad checks failed: ' + ', '.join(failed))
    print('The PS5 bus HID gamepad checks passed' + (' (hosted, PE32 and PE64)' if args.hosted else ''))


if __name__ == '__main__':
    main()
