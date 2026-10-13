#!/usr/bin/env python3
"""Fail closed before executing unverified media dependency sources."""
import json
import importlib.util
import shutil
from unittest.mock import patch
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/build_media_ps5.py'

VIDDEC_FIXTURE = r"""
#include <stdint.h>
#include <stdio.h>
#define GST_CLOCK_TIME_NONE UINT64_MAX
struct Frame { uint64_t pts; };
struct Picture { int pending, released; };
struct Decoder { struct Picture *picture; uint64_t previous; };
#define GST_VIDEO_DECODER(decoder) (decoder)
#define GST_VIDEO_CODEC_FRAME_FLAG_UNSET(frame,flag) ((void)(frame))
int gst_video_decoder_get_subframe_mode(struct Decoder *decoder) { (void)decoder; return 0; }
void gst_video_decoder_have_last_subframe(struct Decoder *decoder,struct Frame *frame) { (void)decoder; (void)frame; }
int gst_video_decoder_finish_frame(struct Decoder *decoder,struct Frame *frame) {
  if(frame->pts == GST_CLOCK_TIME_NONE)
    frame->pts = decoder->picture->pending ? decoder->previous + 33333334 : decoder->previous;
  return 0;
}
void av_frame_unref(struct Picture *picture) { picture->pending=0; ++picture->released; }
int finish(struct Decoder *ffmpegdec,struct Frame *output_frame) {
  struct Frame *input_frame=NULL;
  int result=1,*ret=&result;
  av_frame_unref (ffmpegdec->picture);

  if (input_frame)
    GST_VIDEO_CODEC_FRAME_FLAG_UNSET (input_frame,
        GST_FFMPEG_VIDEO_CODEC_FRAME_FLAG_ALLOCATED);
  if (gst_video_decoder_get_subframe_mode (GST_VIDEO_DECODER (ffmpegdec)))
    gst_video_decoder_have_last_subframe (GST_VIDEO_DECODER (ffmpegdec), output_frame);
  *ret =
      gst_video_decoder_finish_frame (GST_VIDEO_DECODER (ffmpegdec),
      output_frame);

beach:
  return result;
}
int main(void) {
  struct Picture picture={1,0}; struct Decoder decoder={&picture,943355666};
  struct Frame frame={GST_CLOCK_TIME_NONE};
  int result=finish(&decoder,&frame);
  printf("%d %llu %d %d\n",result,(unsigned long long)frame.pts,picture.pending,picture.released);
  return 0;
}
"""

CODECMAP_FIXTURE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int gint;
#define AV_CODEC_ID_ADPCM_IMA_WAV 1
#define AV_CODEC_ID_ADPCM_G726 2
struct Context { int bits_per_coded_sample, sample_rate, bit_rate; };
struct Caps { int present, depth; };
int gst_structure_get_int(struct Caps *str, const char *key, int *value) {
  if (!str->present || strcmp(key, "depth")) return 0;
  *value = str->depth;
  return 1;
}
void adapt(int codec_id, struct Context *context, struct Caps *str) {
  switch (codec_id) {
    case AV_CODEC_ID_ADPCM_G726:
      if (context->sample_rate && context->bit_rate)
        context->bits_per_coded_sample =
            context->bit_rate / context->sample_rate;
      break;
    default:
      break;
  }
}
int main(int argc, char **argv) {
  if (argc != 5) return 2;
  struct Context context = {atoi(argv[2]), 8000, 32000};
  struct Caps caps = {atoi(argv[3]), atoi(argv[4])};
  adapt(atoi(argv[1]), &context, &caps);
  printf("%d\n", context.bits_per_coded_sample);
  return 0;
}
/* Existing WMA metadata anchor:
          "wmaversion", G_TYPE_INT, 3, NULL);
*/
"""
SEEK_ANCHOR = ('\n/* Existing seek call anchor:\n'
               '  if ((seekret =\n'
               '          av_seek_frame (demux->context, index, fftarget,\n'
               '*/\n')
class MediaBuildTests(unittest.TestCase):
    def run_build(self, *args):
        return subprocess.run(['python3', str(SCRIPT), *map(str,args)], capture_output=True, text=True)
    def test_missing_sdk_does_not_create_work(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);r=self.run_build('--sdk',p/'missing','--work',p/'work')
            self.assertNotEqual(r.returncode,0)
            self.assertIn('no PS5 payload SDK',r.stderr)
            self.assertFalse((p/'work').exists())
    def test_corrupt_archive_is_not_extracted_or_built(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);sdk=p/'sdk';(sdk/'bin').mkdir(parents=True)
            cc=sdk/'bin/prospero-clang';cc.write_text('#!/bin/sh\nexit 99\n');cc.chmod(0o755)
            work=p/'work';(work/'downloads').mkdir(parents=True)
            lock=json.loads((ROOT/'tools/media-ps5-sources.json').read_text())
            (work/'downloads'/lock['ffmpeg']['archive']).write_bytes(b'corrupt download')
            r=self.run_build('--sdk',sdk,'--work',work,'--component','ffmpeg','--offline')
            self.assertNotEqual(r.returncode,0)
            self.assertIn('SHA-256 mismatch',r.stderr)
            self.assertFalse((work/'sources').exists())
            self.assertFalse((work/'ffmpeg-manifest.json').exists())
    def test_dependency_stages_verify_archive_before_configure(self):
        for component in ('libffi','pcre2','zlib','libiconv','glib','gstreamer','gst-plugins-base','gst-plugins-good','gst-libav','gst-plugins-bad'):
            with self.subTest(component=component), tempfile.TemporaryDirectory() as d:
                p=Path(d);sdk=p/'sdk';(sdk/'bin').mkdir(parents=True)
                cc=sdk/'bin/prospero-clang';cc.write_text('#!/bin/sh\nexit 99\n');cc.chmod(0o755)
                work=p/'work';(work/'downloads').mkdir(parents=True)
                lock=json.loads((ROOT/'tools/media-ps5-sources.json').read_text())
                (work/'downloads'/lock[component]['archive']).write_bytes(b'corrupt download')
                r=self.run_build('--sdk',sdk,'--work',work,'--component',component,'--offline')
                self.assertIn('SHA-256 mismatch',r.stderr)
                self.assertFalse((work/'sources').exists())
    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required for registration regression')
    def test_mpeg_ps_demux_registers_without_enabling_transport_stream(self):
        spec=importlib.util.spec_from_file_location('media_builder',SCRIPT)
        builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
        # Exercise the real builder's source transformation, then execute the
        # upstream allowlist/continue decision. Factory inventory alone cannot
        # distinguish a configured FFmpeg demuxer from a registered Gst factory.
        registration = r"""
#include <stdio.h>
#include <string.h>
#define GST_RANK_NONE 0
#define GST_RANK_MARGINAL 64
int main(void) {
  const char *names[] = {"asf", "mpeg", "mpegts", "wsvqa", "unknown"};
  struct { const char *name; } format, *in_plugin = &format;
  for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
    int rank;
    in_plugin->name = names[i];
    if (!strcmp (in_plugin->name, "wsvqa") ||
        !strcmp (in_plugin->name, "wsaud")) {
      rank = GST_RANK_MARGINAL;
    } else {
      rank = GST_RANK_NONE;
      continue;
    }
    printf("%s %d\n", in_plugin->name, rank);
  }
  return 0;
}
/* Existing independent source patch anchors:
if (G_UNLIKELY (pts < 0))
  GST_BUFFER_TIMESTAMP (outbuf) = timestamp;
*/
"""
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);source=directory/'source';libav=source/'ext/libav'
            libav.mkdir(parents=True)
            (libav/'gstavdemux.c').write_text(registration+SEEK_ANCHOR)
            (libav/'gstavcodecmap.c').write_text(CODECMAP_FIXTURE)
            (libav/'gstavviddec.c').write_text(VIDDEC_FIXTURE)
            (source/'COPYING').write_text('fixture license')
            work=directory/'work'
            def install(*args,**kwargs):
                output=work/'root/lib/gstreamer-1.0/libgstlibav.a'
                output.parent.mkdir(parents=True);output.write_bytes(b'fixture archive')
            with patch.object(builder,'meson_cross',return_value=(directory/'cross',{})), \
                 patch.object(builder,'run'), patch.object(builder,'install_tree',side_effect=install):
                builder.build_gstreamer('gst-libav',source,directory/'sdk',work,1)
            executable=directory/'registration'
            subprocess.run(['cc',str(libav/'gstavdemux.c'),'-o',str(executable)],check=True)
            registered=dict(line.split() for line in subprocess.check_output([str(executable)],text=True).splitlines())
            self.assertEqual(registered,{'asf':'64','mpeg':'64','wsvqa':'64'})

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required for caps regression')
    def test_ima_wav_caps_use_explicit_depth_or_missing_four_bit_default(self):
        spec=importlib.util.spec_from_file_location('media_builder',SCRIPT)
        builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);source=directory/'source';libav=source/'ext/libav'
            libav.mkdir(parents=True)
            (libav/'gstavdemux.c').write_text('    if (!strcmp (in_plugin->name, "wsvqa") ||\n'
                                           'if (G_UNLIKELY (pts < 0))\n'
                                           '  GST_BUFFER_TIMESTAMP (outbuf) = timestamp;'+SEEK_ANCHOR)
            (libav/'gstavcodecmap.c').write_text(CODECMAP_FIXTURE)
            (libav/'gstavviddec.c').write_text(VIDDEC_FIXTURE)
            (source/'COPYING').write_text('fixture license');work=directory/'work'
            def install(*args,**kwargs):
                output=work/'root/lib/gstreamer-1.0/libgstlibav.a'
                output.parent.mkdir(parents=True);output.write_bytes(b'fixture archive')
            with patch.object(builder,'meson_cross',return_value=(directory/'cross',{})), \
                 patch.object(builder,'run'), patch.object(builder,'install_tree',side_effect=install):
                builder.build_gstreamer('gst-libav',source,directory/'sdk',work,1)
            executable=directory/'caps'
            subprocess.run(['cc',str(libav/'gstavcodecmap.c'),'-o',str(executable)],check=True)
            def mapped(codec,context_depth,present,depth):
                return int(subprocess.check_output([str(executable),*map(str,(codec,context_depth,present,depth))],text=True))
            self.assertEqual(mapped(1,0,0,0),4)
            for depth in (2,3,4,5):
                self.assertEqual(mapped(1,0,1,depth),depth)
                self.assertEqual(mapped(1,depth,0,0),depth)
            for invalid in (0,1,6):
                self.assertEqual(mapped(1,0,1,invalid),invalid)
            self.assertEqual(mapped(1,6,0,0),6)
            self.assertEqual(mapped(2,0,0,0),4)  # Existing G726 bitrate mapping.
            self.assertEqual(mapped(99,0,0,0),0)

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required for timestamp regression')
    def test_demux_dts_uses_pts_start_time_domain_and_preserves_unknown(self):
        spec=importlib.util.spec_from_file_location('media_builder',SCRIPT)
        builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
        demux_source=r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint64_t GstClockTime;
#define GST_CLOCK_TIME_NONE UINT64_MAX
#define GST_CLOCK_TIME_IS_VALID(value) ((value) != GST_CLOCK_TIME_NONE)
#define AV_NOPTS_VALUE INT64_MIN
struct Buffer { GstClockTime pts, dts; };
#define GST_BUFFER_TIMESTAMP(buffer) ((buffer)->pts)
#define GST_BUFFER_DTS(buffer) ((buffer)->dts)
GstClockTime gst_ffmpeg_time_ff_to_gst(int64_t value, int time_base) {
  (void)time_base;
  return value == AV_NOPTS_VALUE ? GST_CLOCK_TIME_NONE : (GstClockTime)value;
}
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  struct { int64_t dts; } pkt = {strtoll(argv[1],NULL,10)};
  struct { int time_base; } stream = {1}, *avstream = &stream;
  struct { GstClockTime start_time; } state = {strtoull(argv[2],NULL,10)}, *demux = &state;
  struct Buffer buffer = {0, GST_CLOCK_TIME_NONE}, *outbuf = &buffer;
  GstClockTime timestamp = 10022333;
  GST_BUFFER_TIMESTAMP (outbuf) = timestamp;
  printf("%llu %llu\n", (unsigned long long)buffer.pts, (unsigned long long)buffer.dts);
  return 0;
}
/* Existing source patch anchors:
    if (!strcmp (in_plugin->name, "wsvqa") ||
if (G_UNLIKELY (pts < 0))
*/
"""
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);source=directory/'source';libav=source/'ext/libav'
            libav.mkdir(parents=True)
            (libav/'gstavdemux.c').write_text(demux_source+SEEK_ANCHOR)
            (libav/'gstavcodecmap.c').write_text(CODECMAP_FIXTURE)
            (libav/'gstavviddec.c').write_text(VIDDEC_FIXTURE)
            (source/'COPYING').write_text('fixture license');work=directory/'work'
            def install(*args,**kwargs):
                output=work/'root/lib/gstreamer-1.0/libgstlibav.a'
                output.parent.mkdir(parents=True);output.write_bytes(b'fixture archive')
            with patch.object(builder,'meson_cross',return_value=(directory/'cross',{})), \
                 patch.object(builder,'run'), patch.object(builder,'install_tree',side_effect=install):
                builder.build_gstreamer('gst-libav',source,directory/'sdk',work,1)
            executable=directory/'timestamps'
            subprocess.run(['cc',str(libav/'gstavdemux.c'),'-o',str(executable)],check=True)
            def mapped(dts,start):
                return tuple(map(int,subprocess.check_output([str(executable),str(dts),str(start)],text=True).split()))
            self.assertEqual(mapped(566666667,523311000),(10022333,43355667))
            self.assertEqual(mapped(500000000,523311000),(10022333,0))
            self.assertEqual(mapped(523311000,523311000),(10022333,0))
            self.assertEqual(mapped(566666667,0),(10022333,566666667))
            self.assertEqual(mapped(-(1<<63),523311000),(10022333,(1<<64)-1))

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required for seek regression')
    def test_mpeg_ps_prerolls_from_origin_without_changing_requested_segment(self):
        spec=importlib.util.spec_from_file_location('media_builder',SCRIPT)
        builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
        seek_source=r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define AVSEEK_FLAG_BACKWARD 1
struct Format { const char *name; };
struct Context { struct Format *iformat; };
struct Demux { struct Context *context; int64_t start_time; };
int64_t actual_seek;
int64_t gst_ffmpeg_time_gst_to_ff(int64_t value,int time_base) { (void)time_base; return value; }
int av_seek_frame(struct Context *context,int index,int64_t target,int flags) {
  (void)context; (void)index; (void)flags; actual_seek=target; return 0;
}
int main(int argc,char **argv) {
  if (argc != 2) return 2;
  struct Format format={argv[1]};struct Context context={&format};
  struct Demux state={&context,523311000},*demux=&state;
  struct { int time_base; } stream_data={1},*stream=&stream_data;
  int64_t requested=756000000,target=requested+demux->start_time;
  int64_t fftarget=gst_ffmpeg_time_gst_to_ff(target,stream->time_base);
  int index=0,seekret;
  if ((seekret =
          av_seek_frame (demux->context, index, fftarget,
              AVSEEK_FLAG_BACKWARD)) < 0) return 3;
  printf("%lld %lld\n",(long long)actual_seek,(long long)(target-demux->start_time));
  return 0;
}
/* Existing independent source patch anchors:
    if (!strcmp (in_plugin->name, "wsvqa") ||
if (G_UNLIKELY (pts < 0))
  GST_BUFFER_TIMESTAMP (outbuf) = timestamp;
*/
"""
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);source=directory/'source';libav=source/'ext/libav'
            libav.mkdir(parents=True)
            (libav/'gstavdemux.c').write_text(seek_source)
            (libav/'gstavcodecmap.c').write_text(CODECMAP_FIXTURE)
            (libav/'gstavviddec.c').write_text(VIDDEC_FIXTURE)
            (source/'COPYING').write_text('fixture license');work=directory/'work'
            def install(*args,**kwargs):
                output=work/'root/lib/gstreamer-1.0/libgstlibav.a'
                output.parent.mkdir(parents=True);output.write_bytes(b'fixture archive')
            with patch.object(builder,'meson_cross',return_value=(directory/'cross',{})), \
                 patch.object(builder,'run'), patch.object(builder,'install_tree',side_effect=install):
                builder.build_gstreamer('gst-libav',source,directory/'sdk',work,1)
            executable=directory/'seek'
            subprocess.run(['cc',str(libav/'gstavdemux.c'),'-o',str(executable)],check=True)
            def mapped(format):
                return tuple(map(int,subprocess.check_output([str(executable),format],text=True).split()))
            self.assertEqual(mapped('mpeg'),(523311000,756000000))
            for other in ('asf','mov','mpegts'):
                self.assertEqual(mapped(other),(1279311000,756000000))

    def test_offline_missing_archive_is_explicit(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);sdk=p/'sdk';(sdk/'bin').mkdir(parents=True)
            cc=sdk/'bin/prospero-clang';cc.write_text('#!/bin/sh\nexit 99\n');cc.chmod(0o755)
            r=self.run_build('--sdk',sdk,'--work',p/'work','--offline')
            self.assertNotEqual(r.returncode,0)
            self.assertIn('offline archive missing',r.stderr)

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required for frame lifetime regression')
    def test_final_frame_remains_pending_until_timestamp_recovery_finishes(self):
        spec=importlib.util.spec_from_file_location('media_builder',SCRIPT)
        builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);source=directory/'source';libav=source/'ext/libav'
            libav.mkdir(parents=True)
            (libav/'gstavdemux.c').write_text('    if (!strcmp (in_plugin->name, "wsvqa") ||\n'
                                           'if (G_UNLIKELY (pts < 0))\n'
                                           '  GST_BUFFER_TIMESTAMP (outbuf) = timestamp;'+SEEK_ANCHOR)
            (libav/'gstavcodecmap.c').write_text(CODECMAP_FIXTURE)
            (libav/'gstavviddec.c').write_text(VIDDEC_FIXTURE)
            (source/'COPYING').write_text('fixture license');work=directory/'work'
            def install(*args,**kwargs):
                output=work/'root/lib/gstreamer-1.0/libgstlibav.a'
                output.parent.mkdir(parents=True);output.write_bytes(b'fixture archive')
            with patch.object(builder,'meson_cross',return_value=(directory/'cross',{})), \
                 patch.object(builder,'run'), patch.object(builder,'install_tree',side_effect=install):
                builder.build_gstreamer('gst-libav',source,directory/'sdk',work,1)
            executable=directory/'finish'
            subprocess.run(['cc',str(libav/'gstavviddec.c'),'-o',str(executable)],check=True)
            result=subprocess.check_output([str(executable)],text=True).split()
            self.assertEqual(result,['0','976689000','0','1'])

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required for RIFF regression')
    def test_ima_adpcm_timing_includes_header_predictor_sample(self):
        spec=importlib.util.spec_from_file_location('media_builder',SCRIPT)
        builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
        fixture = r"""
#include <stdint.h>
#include <stdio.h>
struct Format { unsigned channels, rate, blockalign, av_bps; };
uint64_t gst_util_uint64_scale_int(uint64_t rate, unsigned block, unsigned samples) {
  return rate * block / samples;
}
#define GST_DEBUG(...) ((void)0)
unsigned bitrate(unsigned channels, unsigned rate, unsigned blockalign) {
  struct Format format={channels,rate,blockalign,0},*strf=&format;
        if (strf->channels != 0 && strf->rate != 0 && strf->blockalign != 0) {
          int spb = ((strf->blockalign - strf->channels * 4) / 2) * 2;
          strf->av_bps =
              gst_util_uint64_scale_int (strf->rate, strf->blockalign, spb);
          GST_DEBUG ("fixing av_bps to calculated value %d of IMA DVI ADPCM",
              strf->av_bps);
        }
  return strf->av_bps;
}
int main(void) {
  printf("%u %u %u %u %u %u\n",bitrate(2,48000,1024),bitrate(1,48000,1024),
    bitrate(2,48000,8),bitrate(2,48000,7),bitrate(0,48000,1024),bitrate(2,0,1024));
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory);source=directory/'source'
            riff=source/'gst-libs/gst/riff';riff.mkdir(parents=True)
            (riff/'riff-media.c').write_text(fixture)
            (source/'COPYING').write_text('fixture license');work=directory/'work'
            def install(*args,**kwargs):
                root=work/'root/lib';root.mkdir(parents=True)
                for part in ('allocators','app','audio','fft','pbutils','riff','rtp','rtsp','sdp','tag','video'):
                    (root/('libgst'+part+'-1.0.a')).write_bytes(b'fixture archive')
                plugins=root/'gstreamer-1.0';plugins.mkdir()
                for part in ('app','audioconvert','audioresample','playback','typefindfunctions','videoconvertscale'):
                    (plugins/('libgst'+part+'.a')).write_bytes(b'fixture archive')
            with patch.object(builder,'meson_cross',return_value=(directory/'cross',{})), \
                 patch.object(builder,'run'), patch.object(builder,'install_tree',side_effect=install):
                builder.build_gstreamer('gst-plugins-base',source,directory/'sdk',work,1)
            executable=directory/'riff'
            subprocess.run(['cc',str(riff/'riff-media.c'),'-o',str(executable)],check=True)
            result=subprocess.check_output([str(executable)],text=True).split()
            # Stereo1024 decodes1017 sampleframes, mono1024 decodes2041.
            self.assertEqual(result,['48330','24082','384000','0','0','0'])
            with patch.object(builder,'meson_cross',return_value=(directory/'cross',{})), \
                 self.assertRaisesRegex(RuntimeError,'RIFF IMA/DVI timing patch no longer matches'):
                builder.build_gstreamer('gst-plugins-base',source,directory/'sdk',work,1)

if __name__=='__main__':unittest.main()
