/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Standalone decoder evidence: same source can link to host or PS5 FFmpeg.
 * Decode to buffers; no Wine, display, or audio-device success is implied.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>

typedef struct Probe {
    AVCodecContext *decoder;
    enum AVMediaType type;
    int stream, width, height, monotonic;
    int64_t last_pts;
    uint64_t frames, samples, luma;
} Probe;

static int receive_frames(Probe *probe, AVFrame *frame)
{
    int result;
    while ((result = avcodec_receive_frame(probe->decoder, frame)) >= 0) {
        if (probe->type == AVMEDIA_TYPE_VIDEO) {
            int64_t pts = frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE || (probe->last_pts != AV_NOPTS_VALUE && pts <= probe->last_pts))
                probe->monotonic = 0;
            probe->last_pts = pts;
            probe->width = frame->width;
            probe->height = frame->height;
            /* Our generated H.264 fixture is planar 8-bit YUV420. Reject an
             * unexpected output instead of treating another format as luma. */
            if (frame->format != AV_PIX_FMT_YUV420P) return AVERROR(EINVAL);
            for (int y = 0; y < frame->height; ++y)
                for (int x = 0; x < frame->width; ++x)
                    probe->luma += frame->data[0][y * frame->linesize[0] + x];
            ++probe->frames;
        } else {
            probe->samples += frame->nb_samples;
        }
        av_frame_unref(frame);
    }
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
}

static int open_decoder(AVFormatContext *format, Probe *probe, enum AVMediaType type)
{
    const AVCodec *codec = NULL;
    int result = av_find_best_stream(format, type, -1, -1, &codec, 0);
    if (result < 0) return result;
    probe->stream = result;
    probe->type = type;
    probe->last_pts = AV_NOPTS_VALUE;
    probe->monotonic = 1;
    probe->decoder = avcodec_alloc_context3(codec);
    if (!probe->decoder) return AVERROR(ENOMEM);
    result = avcodec_parameters_to_context(probe->decoder, format->streams[probe->stream]->codecpar);
    if (result < 0) return result;
    probe->decoder->thread_count = 2;
    return avcodec_open2(probe->decoder, codec, NULL);
}

int main(int argc, char **argv)
{
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    Probe video = {0}, audio = {0};
    int result = 0;
    const char *path = argc > 1 ? argv[1] : "/data/prospero-win/media-probe/clip.mp4";
    if (argc < 2) {
        if (!freopen("/data/prospero-win/media-probe/result.json", "w", stdout)) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        if (!freopen("/data/prospero-win/media-probe/trace.log", "w", stderr)) return 2;
        setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "opening %s\n", path);
    }
    if ((result = avformat_open_input(&format, path, NULL, NULL)) < 0) goto cleanup;
    if ((result = avformat_find_stream_info(format, NULL)) < 0) goto cleanup;
    if ((result = open_decoder(format, &video, AVMEDIA_TYPE_VIDEO)) < 0) goto cleanup;
    if ((result = open_decoder(format, &audio, AVMEDIA_TYPE_AUDIO)) < 0) goto cleanup;
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) { result = AVERROR(ENOMEM); goto cleanup; }
    while ((result = av_read_frame(format, packet)) >= 0) {
        Probe *probe = packet->stream_index == video.stream ? &video :
                       packet->stream_index == audio.stream ? &audio : NULL;
        if (probe) {
            result = avcodec_send_packet(probe->decoder, packet);
            if (result >= 0) result = receive_frames(probe, frame);
        }
        av_packet_unref(packet);
        if (result < 0) goto cleanup;
    }
    if (result != AVERROR_EOF) goto cleanup;
    result = avcodec_send_packet(video.decoder, NULL);
    if (result >= 0) result = receive_frames(&video, frame);
    if (result < 0) goto cleanup;
    result = avcodec_send_packet(audio.decoder, NULL);
    if (result >= 0) result = receive_frames(&audio, frame);
    if (result < 0) goto cleanup;
    if (!video.frames || !audio.samples || !video.monotonic) { result = AVERROR_INVALIDDATA; goto cleanup; }
    printf("{\"video_frames\":%" PRIu64 ",\"width\":%d,\"height\":%d,"
           "\"audio_samples\":%" PRIu64 ",\"video_luma_sum\":%" PRIu64 ","
           "\"video_timestamps_monotonic\":true}\n",
           video.frames, video.width, video.height, audio.samples, video.luma);
cleanup:
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, error, sizeof(error));
        fprintf(stderr, "media decode failed: %s; video_frames=%" PRIu64
                "; audio_samples=%" PRIu64 "; monotonic=%d\n",
                error, video.frames, audio.samples, video.monotonic);
        if (argc < 2) printf("{\"error_code\":%d}\n", result);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&video.decoder);
    avcodec_free_context(&audio.decoder);
    avformat_close_input(&format);
    return result < 0 ? 1 : 0;
}
