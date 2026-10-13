#!/usr/bin/env python3
"""Build metadata can relocate; compiled media must not carry builder paths."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name):
 spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/(name+'.py'))
 result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
builder=module('build_media_ps5');scanner=module('check_media_build_paths')
class MediaPaths(unittest.TestCase):
 def test_staging_rewrites_metadata_only(self):
  with tempfile.TemporaryDirectory() as d:
   work=Path(d);source=work/'source';source.mkdir();payload=b'compiled /nonexistent/prospero-media unchanged'
   def install(command,cwd,env,log):
    self.assertIn('DESTDIR='+str(work/'install-staging'),command)
    installed=work/'install-staging'/builder.RUNTIME_PREFIX.lstrip('/')
    (installed/'lib/pkgconfig').mkdir(parents=True)
    (installed/'lib/pkgconfig/test.pc').write_text('prefix='+builder.RUNTIME_PREFIX+'\n')
    (installed/'lib/test.a').write_bytes(payload)
   with patch.object(builder,'run',install):
    builder.install_tree(['make','install'],source,{},work,work/'install.log')
   self.assertEqual((work/'root/lib/test.a').read_bytes(),payload)
   self.assertEqual((work/'root/lib/pkgconfig/test.pc').read_text(),'prefix='+str(work/'root')+'\n')
   self.assertFalse((work/'install-staging').exists())
 def test_all_compiler_inputs_map_file_and_macro_paths(self):
  flags=builder.target_cflags(Path('/sdk/work'),Path('/builder/work'))
  for path in ('/sdk/work','/builder/work',str(ROOT)):
   for flag in ('-ffile-prefix-map','-fmacro-prefix-map'):
    self.assertIn(flag+'='+path+'=/build/media',flags)
 def test_binary_strings_catch_native_and_pe_paths(self):
  with tempfile.TemporaryDirectory() as d:
   file=Path(d)/'module';file.write_bytes(b'\x00/Users/builder/source/file.c\x00/home/vm/root/lib\x00/build/media/safe.c\x00/custom/work/source\x00')
   findings=scanner.check(file,('/custom/work',))
   self.assertEqual(len(findings),3)
   self.assertFalse(any('safe.c' in line for line in findings))
if __name__=='__main__':unittest.main()
