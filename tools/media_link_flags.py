#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Explicit target-only static media flags for Wine and the PRX linker."""
import json, os, shlex, subprocess, sys
from pathlib import Path
PLUGINS='coreelements app playback typefindfunctions audioconvert audioresample videoconvertscale isomp4 videoparsersbad libav deinterlace videofilter avi'.split()
PACKAGES='gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-pbutils-1.0 gstreamer-riff-1.0 gstreamer-rtp-1.0 gstreamer-codecparsers-1.0 gstreamer-net-1.0 gstreamer-fft-1.0 gio-2.0 libavfilter libavformat libavcodec libavutil libswscale libswresample'.split()
def flags(root):
 root=Path(root).resolve()
 if any(c.isspace() for c in str(root)): raise ValueError('media root must not contain whitespace')
 env={k:v for k,v in os.environ.items() if not k.startswith('PKG_CONFIG_')}
 env['PKG_CONFIG_LIBDIR']=str(root/'lib/pkgconfig');env['PKG_CONFIG_PATH']=''
 def query(mode):
  values=shlex.split(subprocess.check_output(['pkg-config','--static',mode,*PACKAGES],env=env,text=True))
  for value in values:
   if value.startswith(('-I','-L')):
    if not Path(value[2:]).resolve().is_relative_to(root): raise ValueError('path outside media root: '+value)
   elif value not in ('-pthread','-lm','-ldl','-DGST_STATIC_COMPILATION','-DPCRE2_STATIC','-DG_INTL_STATIC_COMPILATION') and not value.startswith('-l'):
    raise ValueError('unexpected target flag: '+value)
  return values
 cflags=query('--cflags');libs=query('--libs')
 archives=[root/'lib/gstreamer-1.0'/('libgst'+p+'.a') for p in PLUGINS]
 # The SDK supplies math, threading and dynamic-loader interfaces. All other
 # dependency libraries must exist as static archives in this exact prefix.
 for flag in libs:
  if flag.startswith('-l') and flag not in ('-lm','-ldl','-lpthread'):
   archive=root/'lib'/('lib'+flag[2:]+'.a')
   if archive not in archives: archives.append(archive)
 for archive in archives:
  if not archive.is_file() or not archive.stat().st_size: raise ValueError('missing static archive: '+str(archive))
 static=[str(p) for p in archives]
 return dict(cflags=' '.join(cflags),libs=' '.join(['-Wl,--start-group',*static,'-Wl,--end-group','-pthread']),ldlibs=' '.join(['--start-group',*static,'--end-group']))
if __name__=='__main__':
 try: print(json.dumps(flags(sys.argv[1])))
 except (ValueError,subprocess.CalledProcessError) as e: sys.exit(str(e))
