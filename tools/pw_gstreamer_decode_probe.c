/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Decode local media through statically registered GStreamer plugins.
 * This tests sample delivery and timing, not display or audio-device output. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <gst/video/video.h>
GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(app);
GST_PLUGIN_STATIC_DECLARE(playback);
GST_PLUGIN_STATIC_DECLARE(typefindfunctions);
GST_PLUGIN_STATIC_DECLARE(audioconvert);
GST_PLUGIN_STATIC_DECLARE(audioresample);
GST_PLUGIN_STATIC_DECLARE(videoconvertscale);
GST_PLUGIN_STATIC_DECLARE(isomp4);
GST_PLUGIN_STATIC_DECLARE(avi);
GST_PLUGIN_STATIC_DECLARE(videoparsersbad);
GST_PLUGIN_STATIC_DECLARE(libav);
typedef struct Probe {
    GMutex lock;
    GstElement *video_queue, *audio_queue;
    guint64 frames, samples, luma, missing_pts, nonincreasing_pts;
    GstClockTime last_pts;
    double audio_energy;
    gint width, height;
    gboolean video_linked, audio_linked, invalid_sample;
} Probe;
static GstFlowReturn video_sample(GstAppSink *sink, gpointer data)
{
    Probe *p=data;
    GstSample *sample=gst_app_sink_pull_sample(sink);
    GstVideoInfo info;
    GstVideoFrame frame;
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buffer=gst_sample_get_buffer(sample);
    g_mutex_lock(&p->lock);
    if (!gst_video_info_from_caps(&info,gst_sample_get_caps(sample)) ||
        GST_VIDEO_INFO_FORMAT(&info)!=GST_VIDEO_FORMAT_I420 ||
        !gst_video_frame_map(&frame,&info,buffer,GST_MAP_READ)) {
        p->invalid_sample=TRUE;
    } else {
        GstClockTime pts=GST_BUFFER_PTS(buffer);
        if (!GST_CLOCK_TIME_IS_VALID(pts)) ++p->missing_pts;
        else {
            if (GST_CLOCK_TIME_IS_VALID(p->last_pts) && pts<=p->last_pts) ++p->nonincreasing_pts;
            p->last_pts=pts;
        }
        p->width=GST_VIDEO_INFO_WIDTH(&info); p->height=GST_VIDEO_INFO_HEIGHT(&info);
        const guint8 *pixels=GST_VIDEO_FRAME_PLANE_DATA(&frame,0);
        gint stride=GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0);
        for (gint y=0;y<p->height;++y)
            for (gint x=0;x<p->width;++x) p->luma+=pixels[y*stride+x];
        ++p->frames;
        gst_video_frame_unmap(&frame);
    }
    g_mutex_unlock(&p->lock);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
static GstFlowReturn audio_sample(GstAppSink *sink, gpointer data)
{
    Probe *p=data;
    GstSample *sample=gst_app_sink_pull_sample(sink);
    GstAudioInfo info;
    GstMapInfo map;
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *buffer=gst_sample_get_buffer(sample);
    g_mutex_lock(&p->lock);
    if (!gst_audio_info_from_caps(&info,gst_sample_get_caps(sample)) ||
        GST_AUDIO_INFO_FORMAT(&info)!=GST_AUDIO_FORMAT_F32LE ||
        !gst_buffer_map(buffer,&map,GST_MAP_READ)) {
        p->invalid_sample=TRUE;
    } else {
        const float *values=(const float *)map.data;
        for (gsize i=0;i<map.size/sizeof(float);++i) p->audio_energy+=(double)values[i]*values[i];
        p->samples+=map.size/GST_AUDIO_INFO_BPF(&info);
        gst_buffer_unmap(buffer,&map);
    }
    g_mutex_unlock(&p->lock);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
static void pad_added(GstElement *decoder, GstPad *pad, gpointer data)
{
    (void)decoder;
    Probe *p=data;
    GstCaps *caps=gst_pad_get_current_caps(pad);
    if (!caps || gst_caps_is_empty(caps)) { if(caps)gst_caps_unref(caps);return; }
    const gchar *name=gst_structure_get_name(gst_caps_get_structure(caps,0));
    fprintf(stderr,"decoded pad: %s\n",name);
    gboolean video=g_str_has_prefix(name,"video/x-raw");
    gboolean audio=g_str_has_prefix(name,"audio/x-raw");
    if (video || audio) {
        GstPad *target=gst_element_get_static_pad(video?p->video_queue:p->audio_queue,"sink");
        gboolean linked=gst_pad_link(pad,target)==GST_PAD_LINK_OK;
        g_mutex_lock(&p->lock);
        if (video) p->video_linked=linked; else p->audio_linked=linked;
        g_mutex_unlock(&p->lock);
        gst_object_unref(target);
    }
    gst_caps_unref(caps);
}
static void capabilities(FILE *out)
{
    const char *names[]={"qtdemux","avdemux_asf","avidemux","avdemux_mpeg",
        "avdec_h264","avdec_wmv1","avdec_wmv2","avdec_wmv3","avdec_vc1",
        "avdec_mpeg4","avdec_mpeg1video","avdec_mpeg2video","avdec_aac",
        "avdec_mp2float","avdec_mp3float","avdec_wmav1","avdec_wmav2","avdec_wmapro",
        "avdec_adpcm_ima_wav","avdec_adpcm_ms","avdec_pcm_s16le"};
    fprintf(out,"{\"schema\":\"prospero-media-capabilities/1\",\"playback_verified\":false,\"factories\":{");
    for(guint i=0;i<G_N_ELEMENTS(names);++i) {
        GstElementFactory *factory=gst_element_factory_find(names[i]);
        fprintf(out,"%s\"%s\":%s",i?",":"",names[i],factory?"true":"false");
        if(factory)gst_object_unref(factory);
    }
    fprintf(out,"}}\n");
}
int main(int argc,char **argv)
{
    Probe p={0}; p.last_pts=GST_CLOCK_TIME_NONE; g_mutex_init(&p.lock);
    GError *error=NULL;
    GstElement *pipeline=NULL;
    GstBus *bus=NULL;
    GstMessage *message=NULL;
    int result=1;
    const char *path=argc>1?argv[1]:"/data/prospero-win/media-probe/clip.mp4";
    if (argc<2) {
        if (!freopen("/data/prospero-win/media-probe/gst-decode-result.json","w",stdout)) return 2;
        if (!freopen("/data/prospero-win/media-probe/gst-decode-trace.log","w",stderr)) return 2;
    }
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0","",TRUE);g_setenv("GST_PLUGIN_PATH_1_0","",TRUE);
    g_setenv("GST_REGISTRY","/data/prospero-win/media-probe/gst-registry.bin",TRUE);
    g_setenv("GST_REGISTRY_FORK","no",TRUE);
    g_setenv("GST_REGISTRY_UPDATE","no",TRUE);
    g_setenv("GST_DEBUG","1",FALSE);
    if (!gst_init_check(NULL,NULL,&error)) goto done;
    GST_PLUGIN_STATIC_REGISTER(coreelements);GST_PLUGIN_STATIC_REGISTER(app);
    GST_PLUGIN_STATIC_REGISTER(playback);GST_PLUGIN_STATIC_REGISTER(typefindfunctions);
    GST_PLUGIN_STATIC_REGISTER(audioconvert);GST_PLUGIN_STATIC_REGISTER(audioresample);
    GST_PLUGIN_STATIC_REGISTER(videoconvertscale);GST_PLUGIN_STATIC_REGISTER(isomp4);
    GST_PLUGIN_STATIC_REGISTER(avi);
    GST_PLUGIN_STATIC_REGISTER(videoparsersbad);GST_PLUGIN_STATIC_REGISTER(libav);
    if (argc==2 && !strcmp(argv[1],"--capabilities")) {
        capabilities(stdout); g_mutex_clear(&p.lock); return 0;
    }
    if (argc<2) {
        FILE *out=fopen("/data/prospero-win/media-probe/media-capabilities.json","w");
        if (out) { capabilities(out); fclose(out); }
    }
    const char *required[]={"decodebin","appsink","qtdemux","h264parse","avdec_h264","avdec_aac","avdec_wmv3","avdec_wmapro","avdemux_asf"};
    for (guint i=0;i<G_N_ELEMENTS(required);++i) {
        GstElementFactory *factory=gst_element_factory_find(required[i]);
        fprintf(stderr,"factory %s: %s\n",required[i],factory?"present":"MISSING");
        if (!factory) goto done;
        gst_object_unref(factory);
    }
    pipeline=gst_pipeline_new("decode-probe");
    if (!pipeline) goto done;
    const char *factories[]={"filesrc","decodebin","queue","videoconvert","capsfilter","appsink","queue","audioconvert","capsfilter","appsink"};
    GstElement *e[G_N_ELEMENTS(factories)];
    for(guint i=0;i<G_N_ELEMENTS(e);++i) {
        fprintf(stderr,"creating %s\n",factories[i]);
        e[i]=gst_element_factory_make(factories[i],NULL);
        if(!e[i])goto done;
        gst_bin_add(GST_BIN(pipeline),e[i]);
    }
    p.video_queue=e[2];p.audio_queue=e[6];
    g_object_set(e[0],"location",path,NULL);
    GstCaps *video_caps=gst_caps_from_string("video/x-raw,format=I420");
    GstCaps *audio_caps=gst_caps_from_string("audio/x-raw,format=F32LE,layout=interleaved");
    g_object_set(e[4],"caps",video_caps,NULL);g_object_set(e[8],"caps",audio_caps,NULL);
    gst_caps_unref(video_caps);gst_caps_unref(audio_caps);
    g_object_set(e[5],"emit-signals",TRUE,"sync",FALSE,NULL);
    g_object_set(e[9],"emit-signals",TRUE,"sync",FALSE,NULL);
    g_signal_connect(e[1],"pad-added",G_CALLBACK(pad_added),&p);
    g_signal_connect(e[5],"new-sample",G_CALLBACK(video_sample),&p);
    g_signal_connect(e[9],"new-sample",G_CALLBACK(audio_sample),&p);
    if (!gst_element_link(e[0],e[1]) || !gst_element_link_many(e[2],e[3],e[4],e[5],NULL) ||
        !gst_element_link_many(e[6],e[7],e[8],e[9],NULL)) goto done;
    fprintf(stderr,"branches linked; input %s\n",path);
    bus=gst_element_get_bus(pipeline);
    if (gst_element_set_state(pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE) goto done;
    fprintf(stderr,"PLAYING requested; awaiting EOS\n");
    message=gst_bus_timed_pop_filtered(bus,60*GST_SECOND,GST_MESSAGE_EOS|GST_MESSAGE_ERROR);
    if(message && GST_MESSAGE_TYPE(message)==GST_MESSAGE_ERROR)gst_message_parse_error(message,&error,NULL);
    gst_element_set_state(pipeline,GST_STATE_NULL);
    if(message && GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS && p.video_linked && p.audio_linked &&
       p.frames && p.samples && p.luma && p.audio_energy>0 && !p.invalid_sample && !p.missing_pts && !p.nonincreasing_pts)result=0;
done:
    if(error){fprintf(stderr,"%s\n",error->message);g_error_free(error);}
    if(pipeline)gst_element_set_state(pipeline,GST_STATE_NULL);
    if(message)gst_message_unref(message);if(bus)gst_object_unref(bus);if(pipeline)gst_object_unref(pipeline);
    printf("{\"decode_passed\":%s,\"video_frames\":%" G_GUINT64_FORMAT ",\"audio_samples\":%" G_GUINT64_FORMAT
           ",\"width\":%d,\"height\":%d,\"audio_energy\":%.6f,\"missing_pts\":%" G_GUINT64_FORMAT
           ",\"nonincreasing_pts\":%" G_GUINT64_FORMAT ",\"presentation_verified\":false}\n",
           result?"false":"true",p.frames,p.samples,p.width,p.height,p.audio_energy,p.missing_pts,p.nonincreasing_pts);
    g_mutex_clear(&p.lock);return result;
}
