#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build pinned, target-only media dependencies. FFmpeg stage first.

No runtime-kit installation is performed. A successful build is not evidence
of console playback. Run the native and Wine probes in media-ps5-build.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'tools/media-ps5-sources.json'

def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def fetch_source(name, work, offline):
    pin = json.loads(LOCK.read_text())[name]
    archive = work / 'downloads' / pin['archive']
    if not archive.exists():
        if offline:
            raise RuntimeError(f'offline archive missing: {archive}')
        archive.parent.mkdir(parents=True, exist_ok=True)
        temp = archive.with_suffix(archive.suffix + '.partial')
        with urllib.request.urlopen(pin['url'], timeout=60) as response, temp.open('wb') as out:
            while chunk := response.read(1024 * 1024):
                out.write(chunk)
        if digest(temp) != pin['sha256']:
            raise RuntimeError(f'SHA-256 mismatch: {archive.name}')
        temp.replace(archive)
    if digest(archive) != pin['sha256']:
        raise RuntimeError(f'SHA-256 mismatch: {archive.name}')
    # Never execute a cached edited source tree in place of the pinned archive.
    import tempfile
    sources = work / 'sources'
    sources.mkdir(parents=True, exist_ok=True)
    extraction = Path(tempfile.mkdtemp(prefix=name+'-', dir=sources))
    with tarfile.open(archive) as tar:
        tar.extractall(extraction, filter='data')
    children = list(extraction.iterdir())
    if len(children) != 1 or not children[0].is_dir():
        raise RuntimeError(f'archive must contain one source directory: {archive.name}')
    return children[0], pin

def run(command, cwd, env, log):
    with log.open('w') as out:
        result = subprocess.run(list(map(str,command)), cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}); see {log}')

RUNTIME_PREFIX = '/nonexistent/prospero-media'

def target_cflags(sdk, work):
    # Map headers as well as sources: GStreamer inline diagnostics use __FILE__.
    return ' '.join(['-O2', '-fPIC', *[
        flag + '=' + str(path) + '=/build/media'
        for path in (work, sdk, ROOT)
        for flag in ('-ffile-prefix-map', '-fmacro-prefix-map')]])

def install_tree(command, source, env, work, log, meson=False):
    # Configure runtime defaults with a neutral prefix. Only build metadata
    # receives the actual dependency prefix; no compiled binary is rewritten.
    import shutil
    staging = work / 'install-staging'
    shutil.rmtree(staging, ignore_errors=True)
    if meson:
        command = [*command, '--destdir', str(staging)]
    else:
        command = [*command, 'DESTDIR=' + str(staging)]
    run(command, source, env, log)
    installed = staging / RUNTIME_PREFIX.lstrip('/')
    out = work / 'root'
    shutil.copytree(installed, out, dirs_exist_ok=True)
    for pc in (out / 'lib/pkgconfig').glob('*.pc'):
        pc.write_text(pc.read_text().replace(RUNTIME_PREFIX, str(out)))
    shutil.rmtree(staging)

def build_ffmpeg(source, sdk, work, jobs):
    out = work / 'root'
    (out / 'lib').mkdir(parents=True, exist_ok=True)
    logs = work / 'logs'
    logs.mkdir(exist_ok=True)
    # The payload SDK provides math functions in libc; upstream adds -lm.
    run([sdk/'bin/llvm-ar','rcs',out/'lib/libm.a'],source,os.environ.copy(),logs/'libm.log')
    env = os.environ.copy()
    for key in ('CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','LIBRARY_PATH','PKG_CONFIG_PATH','PKG_CONFIG_SYSROOT_DIR'):
        env.pop(key,None)
    env['PKG_CONFIG_LIBDIR'] = str(out/'lib/pkgconfig')
    args = [source/'configure', '--prefix='+RUNTIME_PREFIX, '--target-os=freebsd', '--arch=x86_64',
            '--enable-cross-compile', '--cc='+str(sdk/'bin/prospero-clang'),
            '--ar='+str(sdk/'bin/llvm-ar'), '--ranlib='+str(sdk/'bin/llvm-ranlib'),
            '--nm='+str(sdk/'bin/llvm-nm'), '--enable-static', '--disable-shared', '--enable-pic',
            '--disable-autodetect', '--disable-programs', '--disable-doc', '--disable-network',
            '--disable-avdevice', '--disable-filters', '--enable-filter=buffer,buffersink,yadif',
            '--disable-postproc', '--disable-encoders',
            '--disable-muxers', '--disable-hwaccels', '--disable-x86asm',
            '--extra-cflags='+target_cflags(sdk,work), '--extra-ldflags=-L'+str(out/'lib')]
    run(args, source, env, logs/'ffmpeg-configure.log')
    # FFmpeg 7.1 has no option to omit its exported configure command. Keep
    # the exact invocation in the manifest, but compile neutral provenance.
    config=source/'config.h'
    import re
    if config.read_text().count('#define FFMPEG_CONFIGURATION ') != 1:
        raise RuntimeError('FFmpeg configuration provenance patch no longer matches')
    config.write_text(re.sub(r'^#define FFMPEG_CONFIGURATION .*$',
        '#define FFMPEG_CONFIGURATION "prospero-media LGPL static build; see source-manifest.json"',
        config.read_text(), flags=re.MULTILINE))
    run(['make','-j'+str(jobs)], source, env, logs/'ffmpeg-build.log')
    install_tree(['make','install'], source, env, work, logs/'ffmpeg-install.log')
    licenses = out/'licenses/ffmpeg'
    licenses.mkdir(parents=True,exist_ok=True)
    import shutil
    for name in ('COPYING.LGPLv2.1','COPYING.LGPLv3','LICENSE.md'):
        shutil.copyfile(source/name,licenses/name)
    archives = [out/'lib'/('lib'+name+'.a') for name in ('avcodec','avformat','avfilter','avutil','swresample','swscale','m')]
    if not all(p.is_file() and p.stat().st_size for p in archives):
        raise RuntimeError('FFmpeg archive inventory is incomplete')
    return {'archives':{str(p.relative_to(out)):digest(p) for p in archives},
            'licenses':{str(p.relative_to(out)):digest(p) for p in licenses.iterdir()},
            'configure':list(map(str,args)), 'source_directory':str(source)}

def build_autotools(name, source, sdk, work, jobs):
    out=work/'root';logs=work/'logs'
    out.mkdir(parents=True,exist_ok=True);logs.mkdir(exist_ok=True)
    env=os.environ.copy()
    for key in ('CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','LIBRARY_PATH','PKG_CONFIG_PATH','PKG_CONFIG_SYSROOT_DIR'):
        env.pop(key,None)
    env.update({'CC':str(sdk/'bin/prospero-clang'), 'AR':str(sdk/'bin/llvm-ar'),
                'RANLIB':str(sdk/'bin/llvm-ranlib'), 'NM':str(sdk/'bin/llvm-nm'),
                'CFLAGS':target_cflags(sdk,work), 'CPPFLAGS':'', 'LDFLAGS':'',
                'PKG_CONFIG_LIBDIR':str(out/'lib/pkgconfig')})
    args=[source/'configure','--host=x86_64-unknown-freebsd11','--prefix='+RUNTIME_PREFIX,
          '--disable-shared','--enable-static']
    if name=='libffi':
        args+=['--disable-multi-os-directory','--disable-docs']
        archives=['libffi.a']; license_names=['LICENSE']
    elif name=='pcre2':
        args+=['--disable-pcre2grep','--disable-tests','--disable-jit']
        archives=['libpcre2-8.a']; license_names=['LICENCE','AUTHORS']
    elif name=='libiconv':
        args+=['--disable-nls']
        archives=['libiconv.a']; license_names=['COPYING.LIB','COPYING']
    elif name=='zlib':
        args=[source/'configure','--prefix='+RUNTIME_PREFIX,'--static']
        env['CHOST']='x86_64-unknown-freebsd11'
        archives=['libz.a']; license_names=['LICENSE']
    else:
        raise RuntimeError(f'unsupported autotools component: {name}')
    run(args,source,env,logs/(name+'-configure.log'))
    if name=='libffi':
        # Clang's PS5 target rejects ms_abi, but libffi's Win64 closure helper
        # must keep that convention. Compile only that helper with the FreeBSD
        # frontend target; retain the SDK headers, PIC/emulated-TLS flags,
        # generated ABI configuration and the normal PS5 link path.
        makefile=source/'x86_64-unknown-freebsd11/Makefile'
        with makefile.open('a') as f:
            f.write('\nsrc/x86/ffiw64.lo: CFLAGS += --target=x86_64-unknown-freebsd11 -mno-red-zone\n')
    run(['make','-j'+str(jobs)],source,env,logs/(name+'-build.log'))
    install_tree(['make','install'],source,env,work,logs/(name+'-install.log'))
    import shutil
    licenses=out/'licenses'/name;licenses.mkdir(parents=True,exist_ok=True)
    for filename in license_names:shutil.copyfile(source/filename,licenses/filename)
    paths=[out/'lib'/filename for filename in archives]
    if not all(p.is_file() and p.stat().st_size for p in paths):
        raise RuntimeError(f'{name} archive inventory is incomplete')
    return {'archives':{str(p.relative_to(out)):digest(p) for p in paths},
            'licenses':{str(p.relative_to(out)):digest(p) for p in licenses.iterdir()},
            'configure':list(map(str,args)), 'source_directory':str(source)}

def meson_cross(source,sdk,out):
    cross=source/'ps5-cross.ini'
    cross.write_text(f"""[binaries]
c = '{sdk}/bin/prospero-clang'
cpp = '{sdk}/bin/prospero-clang++'
ar = '{sdk}/bin/llvm-ar'
strip = '{sdk}/bin/llvm-strip'
pkg-config = '/usr/bin/pkg-config'
[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['{out}/lib/pkgconfig']
[built-in options]
c_args = {target_cflags(sdk,out.parent).split() + ['-I'+str(out/'include')]!r}
cpp_args = {target_cflags(sdk,out.parent).split() + ['-I'+str(out/'include')]!r}
c_link_args = ['-L{out}/lib']
cpp_link_args = ['-L{out}/lib']
""")
    env=os.environ.copy()
    for key in ('CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','LIBRARY_PATH','PKG_CONFIG_PATH','PKG_CONFIG_SYSROOT_DIR','CFLAGS','CPPFLAGS','LDFLAGS','CXXFLAGS'):
        env.pop(key,None)
    env['PKG_CONFIG_LIBDIR']=str(out/'lib/pkgconfig')
    return cross,env

def build_glib(source, sdk, work, jobs, offline):
    import shutil
    out=work/'root'; logs=work/'logs'
    logs.mkdir(parents=True, exist_ok=True)
    # GLib also probes memalign without headers, incorrectly retaining YES
    # after the later malloc.h-aware check fails on the SDK's FreeBSD headers.
    # Leave the header-aware check authoritative; posix_memalign is available.
    meson_file=source/'meson.build'
    meson_text=meson_file.read_text()
    if meson_text.count("  'memalign',\n") != 1:
        raise RuntimeError('GLib memalign probe patch no longer matches')
    meson_file.write_text(meson_text.replace("  'memalign',\n",''))
    proxy,proxy_pin=fetch_source('proxy-libintl',work,offline)
    shutil.copytree(proxy,source/'subprojects/proxy-libintl')
    cross,env=meson_cross(source,sdk,out)
    # GIO links DNS-record APIs even though this kit plays local files only.
    # The SDK declares them without implementations. Keep unsupported queries
    # explicit rather than reporting fabricated successful DNS responses.
    adapter=ROOT/'wine/ps5/pw_media_libc.c'
    adapter_object=work/'pw_media_libc.o'
    resolver=out/'lib/libresolv.a'
    resolver.parent.mkdir(parents=True,exist_ok=True)
    run([sdk/'bin/prospero-clang',*target_cflags(sdk,work).split(),'-c',adapter,'-o',adapter_object],
        source,env,logs/'media-libc-compile.log')
    resolver.unlink(missing_ok=True)
    run([sdk/'bin/llvm-ar','rcs',resolver,adapter_object],source,env,logs/'media-libc-archive.log')
    build=source/'_build'
    args=['meson','setup',build,source,'--cross-file',cross,
          '--prefix='+RUNTIME_PREFIX,'--libdir=lib','--default-library=static',
          '--buildtype=release','--wrap-mode=nodownload','-Dauto_features=disabled',
          '-Dtests=false','-Dinstalled_tests=false','-Ddocumentation=false',
          '-Dxattr=false','-Ddtrace=false','-Dsystemtap=false','-Dnls=disabled']
    run(args,source,env,logs/'glib-configure.log')
    run(['meson','compile','-C',build,'-j',str(jobs)],source,env,logs/'glib-build.log')
    install_tree(['meson','install','-C',build],source,env,work,logs/'glib-install.log',meson=True)
    licenses=out/'licenses/glib';licenses.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source/'COPYING',licenses/'COPYING')
    shutil.copyfile(proxy/'COPYING',licenses/'proxy-libintl-COPYING')
    paths=[out/'lib'/('lib'+name+'.a') for name in ('glib-2.0','gobject-2.0','gmodule-2.0','gio-2.0','gthread-2.0','resolv','intl')]
    if not all(p.is_file() and p.stat().st_size for p in paths):
        raise RuntimeError('GLib archive inventory is incomplete')
    return {'archives':{str(p.relative_to(out)):digest(p) for p in paths},
            'licenses':{str(p.relative_to(out)):digest(p) for p in licenses.iterdir()},
            'configure':list(map(str,args)), 'source_directory':str(source),
            'additional_sources':{'proxy-libintl':proxy_pin},
            'platform_adapter':{'source':'wine/ps5/pw_media_libc.c',
                                'sha256':digest(adapter),'license':'LGPL-2.1-or-later',
                                'dns_records_supported':False,'fstab_enumeration_supported':False}}

def build_gstreamer(name, source, sdk, work, jobs):
    import shutil
    out=work/'root'; logs=work/'logs'
    logs.mkdir(parents=True,exist_ok=True)
    cross,env=meson_cross(source,sdk,out)
    build=source/'_build'
    args=['meson','setup',build,source,'--cross-file',cross,
          '--prefix='+RUNTIME_PREFIX,'--libdir=lib','--default-library=static',
          '--buildtype=release','--wrap-mode=nodownload','-Dauto_features=disabled',
          '-Dtests=disabled','-Ddoc=disabled']
    if name!='gst-libav':
        args+=['-Dexamples=disabled','-Dnls=disabled']
    if name in ('gstreamer','gst-plugins-base','gst-plugins-bad'):
        args+=['-Dtools=disabled','-Dintrospection=disabled']
    if name=='gstreamer':
        args+=['-Dbenchmarks=disabled','-Dptp-helper=disabled','-Dgst_parse=false']
        libraries=['gstreamer-1.0','gstbase-1.0','gstcontroller-1.0','gstnet-1.0']
        plugins=['coreelements']
    elif name=='gst-plugins-base':
        # Backport upstream RIFF IMA/DVI timing: each channel's four-byte
        # block header contributes its predictor sample. Omitting it makes
        # AVI packet timestamps overlap decoded PCM after an accurate seek.
        riff=source/'gst-libs/gst/riff/riff-media.c'
        text=riff.read_text()
        old=('if (strf->channels != 0 && strf->rate != 0 && strf->blockalign != 0) {\n'
             '          int spb = ((strf->blockalign - strf->channels * 4) / 2) * 2;')
        if text.count(old)!=1:
            raise RuntimeError('GStreamer RIFF IMA/DVI timing patch no longer matches')
        riff.write_text(text.replace(old,
            'if (strf->channels != 0 && strf->rate != 0 && strf->blockalign != 0 &&\n'
            '            (strf->blockalign / strf->channels) >= 4) {\n'
            '          int spb = ((strf->blockalign / strf->channels) - 4) * 2 + 1;'))
        plugins=['app','audioconvert','audioresample','playback','typefindfunctions','videoconvertscale']
        args+=['-D'+option+'=enabled' for option in
               ('app','audioconvert','audioresample','playback','typefind','videoconvertscale')]
        args+=['-Dorc=disabled','-Dgl=disabled']
        libraries=['gst'+part+'-1.0' for part in
                   ('allocators','app','audio','fft','pbutils','riff','rtp','rtsp','sdp','tag','video')]
    elif name=='gst-plugins-good':
        plugins=['isomp4','deinterlace','videofilter','avi']; libraries=[]
        args+=['-Disomp4=enabled','-Ddeinterlace=enabled','-Dvideofilter=enabled','-Davi=enabled','-Dorc=disabled']
    elif name=='gst-plugins-bad':
        plugins=['videoparsersbad']; libraries=['gstcodecparsers-1.0']
        args+=['-Dvideoparsers=enabled','-Dorc=disabled']
    elif name=='gst-libav':
        plugins=['libav']; libraries=[]
        # Upstream skips ASF/MPEG-PS because distributions normally ship
        # separate demuxers. This static local-media kit uses FFmpeg for both;
        # GST_RANK_NONE takes a continue and never registers the factory.
        demuxer=source/'ext/libav/gstavdemux.c'
        text=demuxer.read_text()
        marker='    if (!strcmp (in_plugin->name, "wsvqa") ||'
        if text.count(marker)!=1:
            raise RuntimeError('gst-libav ASF registration patch no longer matches')
        demuxer.write_text(text.replace(marker,
            '    if (!strcmp (in_plugin->name, "asf") ||\n'
            '        !strcmp (in_plugin->name, "mpeg") ||\n'
            '        !strcmp (in_plugin->name, "wsvqa") ||'))
        text=demuxer.read_text()
        old='if (G_UNLIKELY (pts < 0))'
        if text.count(old)!=1:
            raise RuntimeError('gst-libav missing-PTS patch no longer matches')
        text=text.replace(old,'if (G_UNLIKELY (pts != AV_NOPTS_VALUE && pts < 0))')
        old='  GST_BUFFER_TIMESTAMP (outbuf) = timestamp;'
        if text.count(old)!=1:
            raise RuntimeError('gst-libav DTS patch no longer matches')
        # PTS is already normalized to context start_time above. MPEG-PS
        # often omits PTS, so parser/decoder fallback DTS must use that same
        # timeline, including upstream's pre-start clamp and unknown sentinel.
        text=text.replace(old,old+'\n  {\n'
            '    GstClockTime dts = gst_ffmpeg_time_ff_to_gst (pkt.dts, avstream->time_base);\n'
            '    if (GST_CLOCK_TIME_IS_VALID (dts)) {\n'
            '      if (demux->start_time >= dts)\n'
            '        dts = 0;\n'
            '      else\n'
            '        dts -= demux->start_time;\n'
            '    }\n'
            '    GST_BUFFER_DTS (outbuf) = dts;\n'
            '  }')
        # FFmpeg's MPEG-PS binary seek selects PES DTS positions, which can
        # land mid-GOP and discard target frames until the next I-frame. Seek
        # from origin and let decoders preroll/clip to the requested segment;
        # this is linear in the MPEG-PS seek position, but frame-correct.
        old=('  if ((seekret =\n'
             '          av_seek_frame (demux->context, index, fftarget,')
        if text.count(old)!=1:
            raise RuntimeError('gst-libav MPEG-PS preroll patch no longer matches')
        text=text.replace(old,
            '  if (!strcmp (demux->context->iformat->name, "mpeg"))\n'
            '    fftarget = gst_ffmpeg_time_gst_to_ff (demux->start_time, stream->time_base);\n\n'+old)
        demuxer.write_text(text)
        # On final FFmpeg drain, dropping the AVFrame can release its codec
        # frame from the pending queue. Keep it alive through finish_frame so
        # GstVideoDecoder can recover missing PTS from that queue before the
        # release callback removes the last frame.
        viddec=source/'ext/libav/gstavviddec.c'
        text=viddec.read_text()
        old='  av_frame_unref (ffmpegdec->picture);\n\n  if (input_frame)'
        finish='      output_frame);\n\nbeach:'
        if text.count(old)!=1 or text.count(finish)!=1:
            raise RuntimeError('gst-libav final-frame lifetime patch no longer matches')
        text=text.replace(old,'  if (input_frame)')
        text=text.replace(finish,'      output_frame);\n  av_frame_unref (ffmpegdec->picture);\n\nbeach:')
        viddec.write_text(text)
        codecmap=source/'ext/libav/gstavcodecmap.c'
        text=codecmap.read_text()
        old='          "wmaversion", G_TYPE_INT, 3, NULL);'
        if text.count(old)!=1:
            raise RuntimeError('gst-libav WMA Pro metadata patch no longer matches')
        text=text.replace(old,old+'\n      if (context)\n'
            '        gst_caps_set_simple (caps, "block_align", G_TYPE_INT, context->block_align,\n'
            '            "bitrate", G_TYPE_INT, (guint) context->bit_rate, NULL);')
        # AVI IMA/DVI caps omit the standard four-bit coded depth. FFmpeg
        # 7.1 rejects an unset value; keep any explicit caps/context depth.
        old=('    case AV_CODEC_ID_ADPCM_G726:\n'
             '      if (context->sample_rate && context->bit_rate)')
        if text.count(old)!=1:
            raise RuntimeError('gst-libav IMA WAV coded-depth patch no longer matches')
        text=text.replace(old,
            '    case AV_CODEC_ID_ADPCM_IMA_WAV:\n'
            '    {\n'
            '      gint depth;\n'
            '      if (gst_structure_get_int (str, "depth", &depth))\n'
            '        context->bits_per_coded_sample = depth;\n'
            '      else if (context->bits_per_coded_sample == 0)\n'
            '        context->bits_per_coded_sample = 4;\n'
            '      break;\n'
            '    }\n'+old)
        codecmap.write_text(text)
    else:
        raise RuntimeError('unsupported GStreamer stage: '+name)
    run(args,source,env,logs/(name+'-configure.log'))
    run(['meson','compile','-C',build,'-j',str(jobs)],source,env,logs/(name+'-build.log'))
    install_tree(['meson','install','-C',build],source,env,work,logs/(name+'-install.log'),meson=True)
    licenses=out/'licenses'/name;licenses.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source/'COPYING',licenses/'COPYING')
    paths=[out/'lib'/('lib'+library+'.a') for library in libraries]
    paths += [out/'lib/gstreamer-1.0'/('libgst'+plugin+'.a') for plugin in plugins]
    if not all(p.is_file() and p.stat().st_size for p in paths):
        raise RuntimeError('GStreamer archive inventory is incomplete')
    return {'archives':{str(p.relative_to(out)):digest(p) for p in paths},
            'licenses':{str(p.relative_to(out)):digest(p) for p in licenses.iterdir()},
            'configure':list(map(str,args)), 'source_directory':str(source),
            'plugins':plugins,'plugin_scanner':False}

def main():
    script_sha = digest(Path(__file__))
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=8)
    parser.add_argument('--component',choices=['ffmpeg','libffi','pcre2','libiconv','zlib','glib','gstreamer','gst-plugins-base','gst-plugins-good','gst-libav','gst-plugins-bad'],default='ffmpeg')
    parser.add_argument('--offline',action='store_true')
    args=parser.parse_args()
    sdk=args.sdk.resolve();work=args.work.resolve()
    if not os.access(sdk/'bin/prospero-clang',os.X_OK):
        raise RuntimeError(f'no PS5 payload SDK at {sdk}')
    if args.jobs < 1:
        raise RuntimeError('--jobs must be positive')
    source,pin=fetch_source(args.component,work,args.offline)
    # A prior successful manifest cannot describe a new in-progress build.
    manifest=work/(args.component+'-manifest.json')
    manifest.unlink(missing_ok=True)
    result=(build_gstreamer(args.component,source,sdk,work,args.jobs) if args.component in ('gstreamer','gst-plugins-base','gst-plugins-good','gst-libav','gst-plugins-bad') else build_glib(source,sdk,work,args.jobs,args.offline) if args.component=='glib' else build_ffmpeg(source,sdk,work,args.jobs) if args.component=='ffmpeg'
            else build_autotools(args.component,source,sdk,work,args.jobs))
    result.update({'schema':'prospero-media-build/1','component':args.component,'source':pin,
                   'sdk_compiler_sha256':digest(sdk/'bin/prospero-clang'),
                   'build_script_sha256':script_sha,
                   'playback_verified':False})
    manifest.write_text(json.dumps(result,indent=2)+'\n')
    print(f'{args.component} static archives built: {manifest}; console playback not yet tested')

if __name__=='__main__':
    try:main()
    except (RuntimeError,OSError,tarfile.TarError) as exc:
        print(f'build_media_ps5: {exc}',file=sys.stderr);sys.exit(1)
