#!/usr/bin/env python3
"""Reject wrong runtime sources before touching the compiler installation."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class RuntimeBuildTests(unittest.TestCase):
    def test_wrong_source_leaves_toolchain_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / 'source'
            source.mkdir()
            subprocess.run(['git', 'init', '-q', str(source)], check=True)
            (source / 'fixture').write_text('wrong source')
            subprocess.run(['git', '-C', str(source), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(source), '-c', 'user.name=Fixture', '-c',
                            'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture'], check=True)
            work = directory / 'work'
            compiler = directory / 'compiler'
            compiler.mkdir()
            sentinel = compiler / 'sentinel'
            sentinel.write_text('unchanged')
            result = subprocess.run(['bash', str(ROOT / 'tools/build_mingw_runtimes.sh'),
                                     '--source', str(source), '--work', str(work),
                                     '--toolchain', str(compiler)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('pinned llvm-mingw source', result.stderr)
            self.assertFalse(work.exists())
            self.assertEqual(sentinel.read_text(), 'unchanged')

if __name__ == '__main__':
    unittest.main()
