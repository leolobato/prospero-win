#!/usr/bin/env python3
"""The local-media resolver adapter must fail explicitly without fabricating DNS data."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class MediaLibcTests(unittest.TestCase):
    def test_dns_records_are_unsupported_and_output_is_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'test.c').write_text('''
#include <assert.h>
#include <errno.h>
#include <netdb.h>
int __res_query(const char *, int, int, unsigned char *, int);
int __dn_expand(const unsigned char *, const unsigned char *, const unsigned char *, char *, int);
struct fstab;
int setfsent(void);
struct fstab *getfsent(void);
void endfsent(void);
int main(void) {
    unsigned char output[16] = {0x7f};
    errno=0; h_errno=0;
    assert(__res_query("synthetic.invalid", 1, 1, output, sizeof(output)) == -1);
    assert(errno == ENOSYS && h_errno == NO_RECOVERY && output[0] == 0x7f);
    errno=0;
    assert(__dn_expand(output, output+16, output, (char *)output, sizeof(output)) == -1);
    assert(errno == ENOSYS && output[0] == 0x7f);
    errno=0;
    assert(setfsent() == 0 && errno == ENOSYS);
    errno=0;
    assert(getfsent() == 0 && errno == ENOSYS);
    endfsent();
    return 0;
}
''')
            built=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(root/'test.c'),
                str(ROOT/'wine/ps5/pw_media_libc.c'),'-o',str(root/'test')],capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stderr)
            result=subprocess.run([str(root/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__': unittest.main()
