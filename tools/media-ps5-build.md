# PS5 static media backend

Build each component with `tools/build_media_ps5.sh --sdk SDK --work WORK
--component COMPONENT --jobs 12`. Use the order FFmpeg, libffi, PCRE2, zlib,
libiconv, GLib, GStreamer, gst-plugins-base, gst-plugins-good,
gst-plugins-bad, gst-libav. `--offline` requires the checksum-pinned archives
in `WORK/downloads`. Fresh extraction prevents cached source edits from
silently replacing a pinned input. Every component emits a manifest containing
archive/license hashes, configure arguments, source identity and compiler hash.

The runtime prefix is `/nonexistent/prospero-media`. Installation uses DESTDIR,
then copies the installed tree into `WORK/root`. Only pkg-config metadata is
rewritten to the build prefix. Compiled binaries are never rewritten. All
stages map source, SDK and repository file/macro paths to `/build/media`,
including inline header diagnostics. NLS is disabled; GLib still builds its
proxy-libintl compatibility library, whose static archive and license are
covered in the manifest and explicit target flags. FFmpeg 7.1 has no configure
option to omit its configuration string: the generated source header replaces
that exported string before compilation with neutral provenance. The full
configure invocation remains in its manifest. Network protocols, encoders,
hardware acceleration, external auto-detection and x86 assembly are disabled.
GPL and nonfree switches are never enabled.

GLib's headerless memalign probe is removed because the SDK provides
posix_memalign and rejects FreeBSD malloc.h inclusion. `pw_media_libc.c` supplies
explicit unsupported errors for DNS record and fstab enumeration entry points.
The SDK math compatibility archive and resolver adapter are included in the
component hash inventory. Static plugin registration bypasses plugin-directory
scanning; registry update and forking are disabled inside patch 0480. No game
environment setup is needed.

`tools/build_wine_ps5.sh --media-root WORK/root` builds matching native and PE
modules. The prefix check rejects host paths and missing static archives.
`pw_media_prx_libc.c` uses an audited subset of SDK libc, excluding syscalls.o.
`media-adapters.json` retains object/source hashes. Unsupported process creation
and atomic ppoll signal-mask changes fail explicitly. Run
`tools/check_media_build_paths.py NATIVE_PRX PE32_DLL PE64_DLL` on the final
artifacts; `--forbid PATH` adds a particular builder/work path. Strict kit
packaging remains the final check, including non-media modules.

The native console probes use `/data/prospero-win/media-probe/`. The GStreamer
probe writes `media-capabilities.json` there on its no-argument console run;
`--capabilities` writes it to stdout on a host. Factory presence advertises a
route, while decoded samples establish format coverage. The final PS5 native
matrix passed all eight generated fixtures with an earlier probe build
`67a891458e660a3caf4b62dd6b3114a1949cc9476785b8d6d5e49a2920c74a30`
and gst-libav archive
`460b60beed305fcb7162eb0ed890d0f1e13274d58e9dfa0a924ab3a94beba765`.
Each produced 30 video frames and audio samples through EOS, with no missing or
nonincreasing video PTS. This establishes native buffer delivery and timing;
display, audio-device output and new-build game acceptance require separate checks.

| Media | Advertised factory or route | Final native console decode evidence |
| --- | --- | --- |
| MP4/MOV | qtdemux | MP4 H.264/AAC; MOV fixture not tested |
| ASF | avdemux_asf | WMV2/WMA v2 and WMV1/WMA v1 |
| AVI | avidemux (gst-good) | MPEG-4 with MP3, IMA WAV ADPCM and PCM |
| MPEG-PS | avdemux_mpeg | MPEG-1/MP2 and MPEG-2/MP2 |
| H.264 | avdec_h264 | MP4 H.264/AAC |
| WMV1/2 | avdec_wmv1, avdec_wmv2 | Both generated ASF fixtures |
| WMV3, VC-1 | avdec_wmv3, avdec_vc1 | Factory presence only for this build |
| MPEG-4 part 2 | avdec_mpeg4 | Three generated AVI fixtures |
| MPEG-1/2 | avdec_mpeg2video | Both generated MPEG-PS fixtures |
| AAC, MP2/MP3 | avdec_aac, avdec_mp2float, avdec_mp3float | MP4, MPEG-PS and AVI fixtures |
| WMA v1/v2 | avdec_wmav1, avdec_wmav2 | Both generated ASF fixtures |
| WMA Pro | avdec_wmapro | Factory presence only for this build |
| IMA WAV ADPCM | avdec_adpcm_ima_wav | MPEG-4/ADPCM AVI |
| MS ADPCM | avdec_adpcm_ms | Factory presence only for this build |
| PCM | avidemux raw-audio output and audioconvert | MPEG-4/PCM AVI |

`avdec_mpeg1video` is absent; MPEG-1 uses the registered MPEG-2 decoder's MPEG-1
caps. `avdec_pcm_s16le` is also absent and unnecessary for AVI raw PCM delivery.
WMV3, VC-1, WMA Pro and MS ADPCM factory presence does not establish decoded
coverage in the new build. Older game evidence below remains historical.

The pinned gst-libav adapter retains its decoded AVFrame through
`gst_video_decoder_finish_frame`, then releases it. Releasing the final drain
frame earlier removed its pending timestamp metadata before missing-PTS recovery,
causing MPEG-1/2's last timestamp to repeat. Matching FFmpeg 7 host reproduction
and the final console matrix verify this lifetime fix without relaxing the probe's
strict timestamp check.

AVI uses the already pinned gst-plugins-good archive, adding no source dependency.
Bink is outside Media Foundation. Games ship its own player. The broad LGPL
FFmpeg set already includes HEVC; use the generated unknown-FourCC MP4 for an
unsupported-codec test rather than assuming HEVC is unsupported.

Set `env = PW_MEDIA_DISABLE=1` in a game's profile `[debug]` section to disable
MF byte-stream handlers and the Windows Media synchronous reader at creation.
The exact value is read once per process. MP4/ASF factories delegate to the
generic handler when Wine's optional DMO demuxer cannot handle the stream, as
on the PS5 build. Host acceptance must use that same fallback path. Creation returns
`MF_E_UNSUPPORTED_BYTESTREAM_TYPE` or `NS_E_UNSUPPORTED_SOURCETYPE`, with null
outputs. Whether a game skips its movie depends on its own error handling.
Patch 0481 preserves asynchronous source failures, rejects empty inputs, checks
read-thread creation, and bounds parser initialization to 30 seconds. Parser
cleanup releases cached reads after failed initialization. Caps replacement and
query ownership use GStreamer's transfer/reference helpers; source arrays take
the descriptor creator reference instead of leaking an extra reference. The caps
warning arose from converting non-fixed template caps as a negotiated format;
the converter now rejects these before calling GStreamer's fixed-caps helpers.
The optional DMO errors precede Wine's fallback to its registered GStreamer
backend; they do not indicate a missing static element when decoding succeeds.

Run `make test-media-runtime` for portable build, path and disable-helper checks.
`make test-media-probe` additionally decodes original generated media using host
FFmpeg; `make test` runs it too but skips it when host FFmpeg is missing.
Cross-compile the Wine probe with `tools/build_mf_probe.sh OUTPUT_DIR`.
Generate short fixtures with `tools/generate_mf_probe_media.py OUTPUT_DIR/media`.
Both PE architectures must run with the runtime under test:

```sh
pw_mf_decode_probe-ARCH.exe h264-aac.mp4
pw_mf_decode_probe-ARCH.exe --ownership h264-aac.mp4
pw_mf_decode_probe-ARCH.exe --expect-error truncated.mp4
pw_mf_decode_probe-ARCH.exe --expect-error unsupported.mp4
pw_mf_decode_probe-ARCH.exe --expect-error empty.mp4
pw_mf_decode_probe-ARCH.exe --expect-error random.mp4
pw_mf_decode_probe-ARCH.exe --cycles h264-aac.mp4
pw_mf_decode_probe-ARCH.exe --cycles wmv-wma.wmv
PW_MEDIA_DISABLE=1 pw_mf_decode_probe-ARCH.exe --disabled
```

The ownership mode retains stream descriptors, shuts down the idle source,
and requires their final external Release to return zero. The normal probe decodes NV12/PCM to EOS, seeks to start and near the end, then
requires EOS again with strictly increasing timestamps within each pass. Near-end
seeking targets three quarters of the duration, including short fixtures. The
reader may decode earlier keyframe preroll, as permitted by Microsoft's
[SetCurrentPosition contract](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-setcurrentposition).
The probe advances through this preroll while checking content, then the
first retained sample must belong to the requested segment (one video frame or
100 ms audio block tolerance); decoded video timestamp/hash/size must match
the first full pass. Start seeking must reproduce its initial video content.
MPEG-PS seeking restarts demuxing at the stream origin and decodes preroll to
the requested segment. FFmpeg's MPEG-PS seek index identifies PES timestamps,
which can land inside a GOP and skip the desired frames until the next I-frame.
The source adapter keeps PTS and DTS on the same origin and lets the decoder
clip preroll. Seek work is linear in the requested position; long MPEG-PS seek
latency has not been measured with the short acceptance fixtures.

Both lower and upper segment bounds apply; stale EOS frames cannot satisfy
the initial segment check. Generated video changes with time so a stale frame
cannot pass a content check. The
cycle mode runs 50 complete open/play/release cycles and 20 partial-play/Flush/
release cycles for both audio and video. Reader release exercises the source
shutdown path. It logs process commit, handles and **Windows** thread counts
every ten cycles. Native GStreamer pthreads require separate OS telemetry; the
Windows snapshot cannot establish those counts. `counters_valid=false` identifies
unavailable resource counters, which block cycle acceptance. The probe uses
class64 handle enumeration, filtered by PID and calibrated with three temporary
events, because Wine's normal process-handle API returns a stubbed zero.
`PW_MF_PROBE_FULL_CYCLES` optionally extends the full-play warm-up, up to 1,000.
Use short generated clips for
cycles; the watchdog exits after 90 seconds without cycle progress. Evidence
must show the counters flatten after warm-up before claiming stability.

Earlier PS5 runs of these probes on a development build delivered H.264/AAC
buffers, seek/repeat and EOS in both PE architectures. Those results do not
validate a new release artifact or cancellation/resource stability. Audio
synchronization is tracked separately. Keep game media local and retain only
hashes, summaries and probe logs.

Patch0482 releases each work queue's shared thread-pool cleanup group once,
closes initialized queues when the handle table is exhausted, and lets the
common initializer own critical-section initialization. Both architecture
`rtworkq.dll` files are rebuilt and staged by the Wine builder; retaining a
baseline DLL would retain these allocation leaks. The focused failure suite
executes actual patched cleanup bodies and forced debug-allocation accounting.
Measure occupied LFH slots separately from `HeapWalk` BUSY bytes: the pinned
Wine implementation reports complete LFH groups, including free inner slots.

PS5 thread alerts own a native kqueue per live Wine thread. Patch 0964 retires
that descriptor on exit or failed creation and serializes alert delivery with
retirement, including TID reuse. Windows handle counters do not include these
native descriptors. `tests/test_wine_alert_kqueue.py` exercises the actual
helpers with real host kqueues and a low descriptor limit; Linux skips this
backend-specific test rather than treating its futex path as kqueue evidence.
