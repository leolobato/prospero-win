#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build a title-safe subset of SDK libc plus the media POSIX adapter.

Print link flags on stdout; retain source/object hashes beside the objects.
Never extract syscalls.o, whose payload-only instructions terminate a title.
"""
import hashlib,json,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
UNITS='backtrace gmtime localtime no-locale passwd strsignal syslog mbsnrtowcs wcsnrtombs arc4random mktemp'.split()
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def build(sdk,out):
 sdk=Path(sdk).resolve();out=Path(out).resolve()
 if any(c.isspace() for c in str(sdk)+str(out)):raise ValueError('adapter paths must not contain whitespace')
 out.mkdir(parents=True,exist_ok=True)
 archive=sdk/'target/lib/libc.a'
 objects=[]
 for unit in UNITS:
  p=out/(unit+'.o')
  p.write_bytes(subprocess.check_output([str(sdk/'bin/llvm-ar'),'p',str(archive),p.name]))
  objects.append(p)
 source=ROOT/'wine/ps5/pw_media_prx_libc.c';adapter=out/'pw_media_prx_libc.o'
 subprocess.run([str(sdk/'bin/prospero-clang'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-fPIC','-c',str(source),'-o',str(adapter)],check=True)
 objects.append(adapter)
 objdump=shutil.which('llvm-objdump-18') or shutil.which('llvm-objdump')
 if not objdump:raise RuntimeError('llvm-objdump is required to audit media adapters')
 import re
 for p in objects:
  code=subprocess.check_output([objdump,'-d','--no-show-raw-insn',str(p)],text=True)
  if re.search(r'\t(?:syscall|sysenter)\b',code):raise RuntimeError('payload-only syscall in '+str(p))
 (out/'media-adapters.json').write_text(json.dumps({'schema':1,'sdk_libc_sha256':digest(archive),'adapter_source_sha256':digest(source),'objects':{p.name:digest(p) for p in objects},'raw_syscalls':0,'locale':'SDK C locale','unsupported':['fork','ppoll with a signal mask','ppoll timeout greater than INT_MAX milliseconds']},indent=2)+'\n')
 # GLib weak sanitizer hooks normally resolve to zero. PRX conversion cannot
 # import undefined weak symbols; resolve them to zero without pretending a
 # sanitizer is present or adding no-op sanitizer functions.
 return ' '.join(map(str,objects))+' --defsym=__lsan_enable=0 --defsym=__lsan_disable=0 --defsym=__lsan_ignore_object=0'
if __name__=='__main__':
 if len(sys.argv)!=3:sys.exit('usage: build_media_prx_adapters.py SDK OUTPUT_DIR')
 print(build(*sys.argv[1:]))
