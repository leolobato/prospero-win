#!/usr/bin/env python3
"""Exercise the native decoder probe against generated, non-game media."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class DecodeProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Host FFmpeg is optional for make test; make test-media-probe requires it.
        missing=[t for t in ('pkg-config','ffmpeg') if not shutil.which(t)]
        if not missing and subprocess.run(['pkg-config','--exists','libavformat','libavcodec','libavutil']).returncode:missing=['FFmpeg development libraries']
        if missing:
            if os.environ.get('PW_REQUIRE_MEDIA_PROBE')=='1':raise RuntimeError('missing '+', '.join(missing))
            raise unittest.SkipTest('missing '+', '.join(missing))
        cls.tmp=tempfile.TemporaryDirectory();cls.path=Path(cls.tmp.name)
        cls.probe=cls.path/'probe';cls.clip=cls.path/'clip.mp4'
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libavformat','libavcodec','libavutil'],text=True))
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tools/pw_media_decode_probe.c'),'-o',str(cls.probe),*flags],check=True)
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=c=red:s=64x48:r=30:d=1','-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=1','-c:v','libx264','-profile:v','baseline','-pix_fmt','yuv420p','-c:a','aac','-ac','2','-movflags','+faststart','-shortest',str(cls.clip)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def test_real_decode_outputs_video_and_audio(self):
        for _ in range(2):
            r=subprocess.run([str(self.probe),str(self.clip)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)
            out=json.loads(r.stdout)
            self.assertEqual(out['video_frames'],30)
            self.assertEqual((out['width'],out['height']),(64,48))
            self.assertGreaterEqual(out['audio_samples'],48000)
            self.assertGreater(out['video_luma_sum'],0)
            self.assertTrue(out['video_timestamps_monotonic'])
    def test_truncated_input_fails_without_success_record(self):
        bad=self.path/'truncated.mp4';bad.write_bytes(self.clip.read_bytes()[:32])
        r=subprocess.run([str(self.probe),str(bad)],capture_output=True,text=True,timeout=5)
        self.assertNotEqual(r.returncode,0)
        self.assertNotIn('video_frames',r.stdout)
if __name__=='__main__':unittest.main()
