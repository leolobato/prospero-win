/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Console architecture gate: static registration, worker threads, GObject
 * signal marshalling and EOS. This does not claim movie playback. */
#include <stdio.h>
#include <gst/gst.h>
GST_PLUGIN_STATIC_DECLARE(coreelements);
static void handoff(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data)
{
    (void)sink; (void)buffer; (void)pad;
    g_atomic_int_inc((gint *)data);
}
int main(void)
{
    GError *error = NULL;
    GstElement *pipeline = NULL, *source = NULL, *sink = NULL;
    GstBus *bus = NULL;
    GstMessage *message = NULL;
    gint buffers = 0;
    int result = 1;
    if (!freopen("/data/prospero-win/media-probe/gst-core-result.json", "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!freopen("/data/prospero-win/media-probe/gst-core-trace.log", "w", stderr)) return 2;
    setvbuf(stderr, NULL, _IONBF, 0);
    g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
    g_setenv("GST_PLUGIN_PATH_1_0", "", TRUE);
    g_setenv("GST_REGISTRY", "/data/prospero-win/media-probe/gst-registry.bin", TRUE);
    g_setenv("GST_REGISTRY_FORK", "no", TRUE);
    fprintf(stderr, "initializing GStreamer\n");
    if (!gst_init_check(NULL, NULL, &error)) goto done;
    GST_PLUGIN_STATIC_REGISTER(coreelements);
    fprintf(stderr, "static registration complete\n");
    pipeline = gst_pipeline_new("core-probe");
    source = gst_element_factory_make("fakesrc", NULL);
    sink = gst_element_factory_make("fakesink", NULL);
    if (!pipeline || !source || !sink) goto done;
    g_object_set(source, "num-buffers", 3, "sizetype", 2, "sizemax", 16, NULL);
    g_object_set(sink, "sync", FALSE, "signal-handoffs", TRUE, NULL);
    g_signal_connect(sink, "handoff", G_CALLBACK(handoff), &buffers);
    gst_bin_add_many(GST_BIN(pipeline), source, sink, NULL);
    source = sink = NULL; /* pipeline owns both */
    source = gst_bin_get_by_name(GST_BIN(pipeline), "fakesrc0");
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "fakesink0");
    if (!source || !sink || !gst_element_link(source, sink)) goto done;
    bus = gst_element_get_bus(pipeline);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) goto done;
    message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
        gst_message_parse_error(message, &error, NULL);
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && g_atomic_int_get(&buffers) == 3)
        result = 0;
done:
    if (error) { fprintf(stderr, "%s\n", error->message); g_error_free(error); }
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (message) gst_message_unref(message);
    if (bus) gst_object_unref(bus);
    if (source) gst_object_unref(source);
    if (sink) gst_object_unref(sink);
    if (pipeline) gst_object_unref(pipeline);
    printf("{\"core_pipeline_passed\":%s,\"buffers\":%d,\"movie_playback_verified\":false}\n",
           result ? "false" : "true", g_atomic_int_get(&buffers));
    return result;
}
