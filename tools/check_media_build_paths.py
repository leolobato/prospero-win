#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Reject builder paths in the native media module and both PE DLLs."""
import argparse
from pathlib import Path
import re
import subprocess

def check(path, forbidden=()):
    strings=subprocess.check_output(['strings',str(path)],text=True,errors='replace')
    return [line for line in strings.splitlines() if
            re.search(r'/(?:home|Users|private/var|var/folders)/',line) or
            any(str(prefix) in line for prefix in forbidden)]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--forbid',action='append',default=[])
    parser.add_argument('native',type=Path)
    parser.add_argument('pe32',type=Path)
    parser.add_argument('pe64',type=Path)
    args=parser.parse_args()
    failed=False
    for path in (args.native,args.pe32,args.pe64):
        findings=check(path,args.forbid)
        if findings:
            failed=True
            print(str(path)+':\n'+'\n'.join(findings))
    return 1 if failed else 0
if __name__=='__main__':raise SystemExit(main())
