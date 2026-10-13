#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate original short media for MF decode, failure and cycle probes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

def generate(out):
    out.mkdir(parents=True,exist_ok=True)
    video=['-f','lavfi','-i','testsrc2=s=64x48:r=30:d=1']
    audio=['-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=1']
    formats={
        'h264-aac.mp4':['-c:v','libx264','-profile:v','baseline','-c:a','aac','-movflags','+faststart'],
        'wmv-wma.wmv':['-c:v','wmv2','-c:a','wmav2'],
        'mpeg4-mp3.avi':['-c:v','mpeg4','-c:a','libmp3lame'],
        'mpeg2-mp2.mpg':['-c:v','mpeg2video','-c:a','mp2'],
        'mpeg4-adpcm.avi':['-c:v','mpeg4','-c:a','adpcm_ima_wav'],
        'mpeg4-pcm.avi':['-c:v','mpeg4','-c:a','pcm_s16le'],
        'mpeg1-mp2.mpg':['-c:v','mpeg1video','-c:a','mp2'],
        'wmv1-wma1.wmv':['-c:v','wmv1','-c:a','wmav1'],
    }
    for filename,codecs in formats.items():
        subprocess.run(['ffmpeg','-v','error','-y',*video,*audio,*codecs,'-pix_fmt','yuv420p','-ac','2','-shortest',str(out/filename)],check=True)
    clip=(out/'h264-aac.mp4').read_bytes()
    (out/'truncated.mp4').write_bytes(clip[:32])
    (out/'empty.mp4').write_bytes(b'')
    (out/'random.mp4').write_bytes(bytes(range(256))*4)
    # An intact MP4 with a deliberately unknown video sample-entry FourCC.
    # Avoid HEVC: the existing broad LGPL FFmpeg decoder set supports it.
    if clip.count(b'avc1')<1:raise RuntimeError('fixture has no AVC sample entry')
    unsupported=out/'unsupported.mp4'
    subprocess.run(['ffmpeg','-v','error','-y',*video,'-c:v','libx264','-profile:v','baseline','-pix_fmt','yuv420p','-an','-movflags','+faststart',str(unsupported)],check=True)
    unsupported.write_bytes(unsupported.read_bytes().replace(b'avc1',b'zzzz'))
    manifest={}
    for path in sorted(out.iterdir()):
        if path.suffix=='.json':continue
        manifest[path.name]={'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'size':path.stat().st_size}
        if path.name in formats:
            manifest[path.name]['ffprobe']=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(path)]))
    (out/'inputs.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('out',type=Path)
    generate(parser.parse_args().out)
