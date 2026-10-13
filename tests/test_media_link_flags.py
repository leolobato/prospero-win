#!/usr/bin/env python3
"""The Wine media link must use only the supplied target prefix."""
import json, os, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Flags(unittest.TestCase):
 def test_target_only_and_missing_archive(self):
  with tempfile.TemporaryDirectory() as d:
   base=Path(d); root=base/'media'; bin=base/'bin'; bin.mkdir()
   lib=root/'lib'; (lib/'gstreamer-1.0').mkdir(parents=True)
   for p in ['coreelements','app','playback','typefindfunctions','audioconvert','audioresample','videoconvertscale','isomp4','videoparsersbad','libav','deinterlace','videofilter','avi']:
    (lib/'gstreamer-1.0'/f'libgst{p}.a').write_bytes(b'target')
   (lib/'libfixture.a').write_bytes(b'target')
   pkg=bin/'pkg-config'
   pkg.write_text('#!/bin/sh\n[ -z "${PKG_CONFIG_PATH:-}" ] || exit 91\n[ "$PKG_CONFIG_LIBDIR" = "$FIXTURE_ROOT/lib/pkgconfig" ] || exit 92\ncase "$*" in *--cflags*) echo "-I$FIXTURE_ROOT/include -DGST_STATIC_COMPILATION -DPCRE2_STATIC -DG_INTL_STATIC_COMPILATION";; *) echo "-L$FIXTURE_ROOT/lib -lfixture -pthread -lm -ldl";; esac\n')
   pkg.chmod(0o755)
   env=dict(os.environ,PATH=str(bin)+os.pathsep+os.environ['PATH'],PKG_CONFIG_PATH='/host/unsafe',FIXTURE_ROOT=str(root.resolve()))
   def run(): return subprocess.run(['python3',str(ROOT/'tools/media_link_flags.py'),str(root)],env=env,capture_output=True,text=True)
   r=run(); self.assertEqual(r.returncode,0,r.stderr)
   flags=json.loads(r.stdout);self.assertIn('-DG_INTL_STATIC_COMPILATION',flags['cflags']);self.assertIn('--start-group',flags['ldlibs']);self.assertNotIn('-pthread',flags['ldlibs'])
   self.assertIn('-Wl,--start-group',flags['libs']);self.assertNotIn('/host',r.stdout)
   (lib/'libfixture.a').unlink()
   r=run();self.assertNotEqual(r.returncode,0);self.assertIn('libfixture.a',r.stderr)
   (lib/'libfixture.a').write_bytes(b'target')
   pkg.write_text('#!/bin/sh\necho "-I/usr/include -L/usr/lib -lfixture"\n')
   r=run();self.assertNotEqual(r.returncode,0);self.assertIn('outside media root',r.stderr)
if __name__=='__main__': unittest.main()
