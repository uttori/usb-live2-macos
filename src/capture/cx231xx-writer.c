// SPDX-License-Identifier: GPL-2.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "cx231xx-writer.h"
#include "cx231xx-raw.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    SAMPLE_RATE = 48000,
    NANOSECONDS = 1000000000,
};

static const AVRational ns_time_base = {1, NANOSECONDS};
static const AVRational frame_rate = {30000, 1001};
static const AVRational field_rate = {60000, 1001};

const char *capture_profile_name(enum capture_profile profile)
{
    static const char *const names[] = {"ffv1", "prores", "h264", "h264-hw", "raw"};
    return names[profile];
}

const char *capture_media_name(enum capture_profile profile, bool complete)
{
    if (profile == CAPTURE_FFV1)
        return complete ? "capture.mkv" : "capture.partial.mkv";
    if (profile == CAPTURE_PRORES || profile == CAPTURE_RAW)
        return complete ? "capture.mov" : "capture.partial.mov";
    return complete ? "capture.mp4" : "capture.partial.mp4";
}

struct capture_writer {
    enum capture_profile profile;
    AVFormatContext *mux;
    AVCodecContext *encoder, *audio_encoder;
    AVStream *video, *audio;
    AVFrame *frame, *filtered, *pcm;
    AVFilterGraph *filter;
    AVFilterContext *filter_in, *filter_out;
    AVPacket *packet;
    char *path;
    int fd;
    bool has_samples;
    uint64_t offset, checkpoint;
    int64_t audio_next_pts;
};

static bool h264_profile(enum capture_profile profile)
{
    return profile == CAPTURE_H264 || profile == CAPTURE_H264_HW;
}

static int configure_video(struct capture_writer *writer)
{
    enum capture_profile profile = writer->profile;
    bool progressive = h264_profile(profile);
    bool raw = profile == CAPTURE_RAW;
    static const char *const encoders[] = {"ffv1", "prores_ks", "libx264", "h264_videotoolbox"};
    const AVCodec *codec = raw ? NULL : avcodec_find_encoder_by_name(encoders[profile]);
    if (!raw && !codec)
        return AVERROR_ENCODER_NOT_FOUND;
    AVCodecContext *enc = writer->encoder = avcodec_alloc_context3(codec);
    AVStream *video = writer->video = avformat_new_stream(writer->mux, NULL);
    if (!enc || !video)
        return AVERROR(ENOMEM);

    enc->width = NTSC_WIDTH;
    enc->height = NTSC_FIELD_LINES * 2;
    enc->pix_fmt = AV_PIX_FMT_YUV422P;
    enc->bits_per_raw_sample = 8;
    enc->time_base = ns_time_base;
    enc->framerate = frame_rate;
    enc->field_order = AV_FIELD_TT;
    // NTSC-M SD metadata describes the samples without changing their levels or geometry.
    enc->sample_aspect_ratio = (AVRational){8, 9};
    enc->color_range = AVCOL_RANGE_MPEG;
    enc->colorspace = AVCOL_SPC_SMPTE170M;
    enc->color_primaries = AVCOL_PRI_SMPTE170M;
    enc->color_trc = AVCOL_TRC_SMPTE170M;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_FRAME_DURATION;
    int rc = 0;
    if (raw) {
        // The MOV yuvs sample entry uses the same packed byte order as the USB reconstruction.
        enc->codec_type = AVMEDIA_TYPE_VIDEO;
        enc->codec_id = AV_CODEC_ID_RAWVIDEO;
        enc->codec_tag = MKTAG('y', 'u', 'v', 's');
        enc->pix_fmt = AV_PIX_FMT_YUYV422;
    } else if (profile == CAPTURE_FFV1) {
        // Version 3 with four CRC-protected slices and roughly one-second GOPs limits error propagation.
        enc->level = 3;
        enc->slices = 4;
        enc->gop_size = 30;
        rc = av_opt_set_int(enc->priv_data, "slicecrc", 1, 0);
    } else if (profile == CAPTURE_PRORES) {
        enc->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        enc->bits_per_raw_sample = 10;
        enc->flags |= AV_CODEC_FLAG_INTERLACED_DCT;
        enc->thread_count = 4; // Bound encoder CPU use so acquisition and preview keep getting scheduled.
        rc = av_opt_set(enc->priv_data, "profile", "hq", 0);
    } else {
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->field_order = AV_FIELD_PROGRESSIVE;
        enc->framerate = field_rate;
        enc->time_base = (AVRational){1, 90000}; // Conventional video clock; field times round to 1501/1502 ticks.
        enc->gop_size = 60;
        enc->max_b_frames = 0;
        if (profile == CAPTURE_H264_HW) {
            enc->bit_rate = 4000000; // Bits/s target for the optional viewing copy; archive with FFV1.
            rc = av_opt_set(enc->priv_data, "realtime", "1", 0);
        } else {
            enc->thread_count = 4;
            rc = av_opt_set(enc->priv_data, "preset", "fast", 0);
            if (rc >= 0)
                rc = av_opt_set(enc->priv_data, "tune", "zerolatency", 0);
            if (rc >= 0)
                rc = av_opt_set(enc->priv_data, "crf", "18", 0);
        }
    }
    if (!raw && rc >= 0)
        rc = avcodec_open2(enc, codec, NULL);
    if (rc < 0)
        return rc;
    video->time_base = enc->time_base;
    video->avg_frame_rate = progressive ? field_rate : frame_rate;
    video->sample_aspect_ratio = enc->sample_aspect_ratio;
    rc = avcodec_parameters_from_context(video->codecpar, enc);
    // The raw profile needs the context only to populate stream metadata.
    if (raw)
        avcodec_free_context(&writer->encoder);
    return rc;
}

static int configure_audio(struct capture_writer *writer)
{
    AVStream *audio = writer->audio = avformat_new_stream(writer->mux, NULL);
    if (!audio)
        return AVERROR(ENOMEM);
    if (!h264_profile(writer->profile)) {
        // Captured samples are already interleaved little-endian PCM and can be muxed directly.
        AVCodecParameters *par = audio->codecpar;
        par->codec_type = AVMEDIA_TYPE_AUDIO;
        par->codec_id = AV_CODEC_ID_PCM_S16LE;
        par->format = AV_SAMPLE_FMT_S16;
        par->sample_rate = SAMPLE_RATE;
        par->bits_per_coded_sample = 16;
        par->block_align = PCM_FRAME_BYTES;
        par->bit_rate = 8 * PCM_FRAME_BYTES * SAMPLE_RATE;
        par->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        audio->time_base = ns_time_base;
        return 0;
    }
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec)
        return AVERROR_ENCODER_NOT_FOUND;
    AVCodecContext *enc = writer->audio_encoder = avcodec_alloc_context3(codec);
    AVFrame *pcm = writer->pcm = av_frame_alloc();
    if (!enc || !pcm)
        return AVERROR(ENOMEM);
    enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc->sample_rate = SAMPLE_RATE;
    enc->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    enc->time_base = audio->time_base = (AVRational){1, SAMPLE_RATE};
    enc->bit_rate = 192000;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int rc = avcodec_open2(enc, codec, NULL);
    if (rc < 0)
        return rc;
    pcm->format = enc->sample_fmt;
    pcm->sample_rate = enc->sample_rate;
    pcm->ch_layout = enc->ch_layout;
    pcm->nb_samples = enc->frame_size;
    rc = av_frame_get_buffer(pcm, 0);
    return rc < 0 ? rc : avcodec_parameters_from_context(audio->codecpar, enc);
}

/** Separate packed YUYV into planes, retaining row order and every sample value. */
static void unpack_video(AVFrame *frame, const uint8_t *packed)
{
    for (int row = 0; row < frame->height; ++row) {
        uint8_t *y = frame->data[0] + row * frame->linesize[0];
        uint8_t *u = frame->data[1] + row * frame->linesize[1];
        uint8_t *v = frame->data[2] + row * frame->linesize[2];
        if (frame->format == AV_PIX_FMT_YUV422P10LE) {
            // Place the original eight bits in the high bits of each ten-bit component.
            uint16_t *y10 = (uint16_t *)y, *u10 = (uint16_t *)u, *v10 = (uint16_t *)v;
            for (int x = 0; x < frame->width / 2; ++x) {
                y10[2 * x] = (uint16_t)(packed[0] << 2);
                u10[x] = (uint16_t)(packed[1] << 2);
                y10[2 * x + 1] = (uint16_t)(packed[2] << 2);
                v10[x] = (uint16_t)(packed[3] << 2);
                packed += 4;
            }
            continue;
        }
        for (int x = 0; x < frame->width / 2; ++x) {
            y[2 * x] = packed[0];
            u[x] = packed[1];
            y[2 * x + 1] = packed[2];
            v[x] = packed[3];
            packed += 4;
        }
    }
}

static int write_packet(AVFormatContext *mux, const AVStream *stream, AVPacket *packet,
                        AVRational time_base)
{
    // The muxer sets the actual stream time base when writing the header.
    av_packet_rescale_ts(packet, time_base, stream->time_base);
    packet->stream_index = stream->index;
    return av_interleaved_write_frame(mux, packet);
}

static int encode(AVFormatContext *mux, const AVStream *stream, AVCodecContext *enc,
                  AVFrame *frame, AVPacket *packet)
{
    int rc = avcodec_send_frame(enc, frame);
    while (rc >= 0) {
        rc = avcodec_receive_packet(enc, packet);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
            return 0;
        if (rc >= 0)
            rc = write_packet(mux, stream, packet, enc->time_base);
    }
    return rc;
}

/** Deinterlace before converting chroma to 4:2:0 so vertically adjacent fields stay separate. */
static int init_filter(struct capture_writer *writer)
{
    writer->filter = avfilter_graph_alloc();
    writer->filtered = av_frame_alloc();
    if (!writer->filter || !writer->filtered)
        return AVERROR(ENOMEM);
    writer->filter->nb_threads = 2;
    AVFilterContext *deinterlace, *format;
    int rc = avfilter_graph_create_filter(&writer->filter_in, avfilter_get_by_name("buffer"), "input",
        "video_size=720x480:pix_fmt=yuv422p:time_base=1/1000000000:pixel_aspect=8/9:"
        "frame_rate=30000/1001:colorspace=smpte170m:range=tv",
        NULL, writer->filter);
    if (rc >= 0)
        rc = avfilter_graph_create_filter(&deinterlace, avfilter_get_by_name("bwdif"), "fields",
            "mode=send_field:parity=tff:deint=all", NULL, writer->filter);
    if (rc >= 0)
        rc = avfilter_graph_create_filter(&format, avfilter_get_by_name("format"), "420",
            "pix_fmts=yuv420p", NULL, writer->filter);
    if (rc >= 0)
        rc = avfilter_graph_create_filter(&writer->filter_out, avfilter_get_by_name("buffersink"), "output",
            NULL, NULL, writer->filter);
    if (rc >= 0)
        rc = avfilter_link(writer->filter_in, 0, deinterlace, 0);
    if (rc >= 0)
        rc = avfilter_link(deinterlace, 0, format, 0);
    if (rc >= 0)
        rc = avfilter_link(format, 0, writer->filter_out, 0);
    return rc < 0 ? rc : avfilter_graph_config(writer->filter, NULL);
}

static int filter_video(struct capture_writer *writer, AVFrame *input)
{
    int rc = av_buffersrc_add_frame_flags(writer->filter_in, input, AV_BUFFERSRC_FLAG_KEEP_REF);
    while (rc >= 0) {
        AVFrame *frame = writer->filtered;
        rc = av_buffersink_get_frame(writer->filter_out, frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
            return 0;
        if (rc < 0)
            return rc;
        AVRational filter_time = av_buffersink_get_time_base(writer->filter_out);
        frame->pts = av_rescale_q(frame->pts, filter_time, writer->encoder->time_base);
        frame->duration = av_rescale_q(1, av_inv_q(field_rate), writer->encoder->time_base);
        rc = encode(writer->mux, writer->video, writer->encoder, frame, writer->packet);
        av_frame_unref(frame);
    }
    return rc;
}

/** AAC consumes 1024-sample blocks. The recorder only sends a short block as its final unit. */
static int encode_pcm(struct capture_writer *writer, const uint8_t *bytes, size_t size, int64_t pts)
{
    AVCodecContext *enc = writer->audio_encoder;
    AVFrame *pcm = writer->pcm;
    size_t count = size / PCM_FRAME_BYTES;
    if (!count || count > (size_t)enc->frame_size)
        return AVERROR(EINVAL);
    int rc = av_frame_make_writable(pcm);
    if (rc < 0)
        return rc;
    for (size_t channel = 0; channel < 2; ++channel) {
        float *samples = (float *)pcm->data[channel];
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *sample = bytes + PCM_FRAME_BYTES * i + 2 * channel;
            int value = sample[0] | (sample[1] << 8);
            if (value >= 32768)
                value -= 65536;
            samples[i] = (float)value / 32768.0f;
        }
    }
    // MP4 AAC has a continuous sample clock. Preserve the measured start without adding gaps.
    if (writer->audio_next_pts == AV_NOPTS_VALUE)
        writer->audio_next_pts = av_rescale_q(pts, ns_time_base, enc->time_base);
    pcm->pts = writer->audio_next_pts;
    writer->audio_next_pts += (int64_t)count;
    pcm->nb_samples = (int)count;
    pcm->duration = (int64_t)count;
    return encode(writer->mux, writer->audio, enc, pcm, writer->packet);
}

static int write_file(void *opaque, const uint8_t *data, int size)
{
    struct capture_writer *writer = opaque;
    int left = size;
    while (left > 0) {
        ssize_t written = write(writer->fd, data, (size_t)left);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return AVERROR(written < 0 ? errno : EIO);
        left -= (int)written;
        data += written;
        writer->offset += (uint64_t)written;
    }
    return size;
}

/** Each successful append ends after complete Matroska elements or ISO BMFF boxes. */
static int checkpoint(struct capture_writer *writer, int rc)
{
    if (rc < 0)
        return rc;
    avio_flush(writer->mux->pb);
    if (writer->mux->pb->error)
        return writer->mux->pb->error;
    writer->checkpoint = writer->offset;
    return 0;
}

/** Allocate the woven input frame only for profiles that run a video encoder. */
static int init_video_frame(struct capture_writer *writer)
{
    writer->frame = av_frame_alloc();
    if (!writer->frame)
        return AVERROR(ENOMEM);
    writer->frame->format = writer->encoder->pix_fmt;
    if (h264_profile(writer->profile)) {
        writer->frame->format = AV_PIX_FMT_YUV422P;
        int rc = init_filter(writer);
        if (rc < 0)
            return rc;
    }
    writer->frame->width = writer->encoder->width;
    writer->frame->height = writer->encoder->height;
    writer->frame->sample_aspect_ratio = writer->encoder->sample_aspect_ratio;
    writer->frame->flags = AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
    writer->frame->color_range = writer->encoder->color_range;
    writer->frame->colorspace = writer->encoder->colorspace;
    writer->frame->color_primaries = writer->encoder->color_primaries;
    writer->frame->color_trc = writer->encoder->color_trc;
    return av_frame_get_buffer(writer->frame, 0);
}

static int init_writer(struct capture_writer *writer)
{
    static const char *const containers[] = {"matroska", "mov", "mp4", "mp4", "mov"};
    int rc = avformat_alloc_output_context2(&writer->mux, NULL, containers[writer->profile], NULL);
    if (rc >= 0)
        rc = configure_video(writer);
    if (rc >= 0)
        rc = configure_audio(writer);
    if (rc >= 0 && writer->encoder)
        rc = init_video_frame(writer);
    if (rc < 0)
        return rc;
    writer->packet = av_packet_alloc();
    if (!writer->packet)
        return AVERROR(ENOMEM);

    const int buffer_size = 64 * 1024;
    uint8_t *buffer = av_malloc(buffer_size);
    if (!buffer)
        return AVERROR(ENOMEM);
    writer->mux->pb = avio_alloc_context(buffer, buffer_size, 1, writer, NULL, write_file, NULL);
    if (!writer->mux->pb) {
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    // shortcut: Append-only output omits duration and cues, add bounded indexing if seeking is needed.
    writer->mux->flags |= AVFMT_FLAG_CUSTOM_IO;
    writer->mux->max_interleave_delta = 250000; // Microseconds: bound how long one track waits for the other.
    if (writer->profile == CAPTURE_FFV1) {
        // Seal clusters at 250 ms or 4 MiB so a failed append loses only a bounded tail.
        rc = av_opt_set_int(writer->mux->priv_data, "cluster_time_limit", 250, 0);
        if (rc >= 0)
            rc = av_opt_set_int(writer->mux->priv_data, "cluster_size_limit", 4 * 1024 * 1024, 0);
        if (rc >= 0)
            rc = av_opt_set_int(writer->mux->priv_data, "live", 1, 0);
    } else {
        // Delay the initial movie box so the muxer can record each track's start and AAC priming.
        rc = av_opt_set(writer->mux->priv_data, "movflags", "+delay_moov+frag_keyframe+skip_trailer", 0);
        if (rc >= 0)
            rc = av_opt_set_int(writer->mux->priv_data, "frag_duration", 1001000, 0); // µs: 30 NTSC frames.
        if (rc >= 0)
            rc = av_opt_set_int(writer->mux->priv_data, "min_frag_duration", 750000, 0);
        if (rc >= 0)
            rc = av_opt_set_int(writer->mux->priv_data, "video_track_timescale", 90000, 0);
    }
    if (rc >= 0)
        rc = avformat_write_header(writer->mux, NULL);
    return checkpoint(writer, rc);
}

int capture_writer_open(struct capture_writer **output, const char *directory, enum capture_profile profile)
{
    struct capture_writer *writer = calloc(1, sizeof *writer);
    char *path = av_asprintf("%s/%s", directory, capture_media_name(profile, false));
    if (!writer || !path) {
        free(writer);
        av_free(path);
        return AVERROR(ENOMEM);
    }
    writer->path = path;
    writer->profile = profile;
    writer->audio_next_pts = AV_NOPTS_VALUE;
    writer->fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    int rc = writer->fd < 0 ? AVERROR(errno) : init_writer(writer);
    if (rc >= 0) {
        *output = writer;
    } else {
        bool finalized;
        uint64_t bytes;
        capture_writer_close(writer, rc, &finalized, &bytes);
    }
    return rc;
}

int capture_writer_write(struct capture_writer *writer, bool video, const uint8_t *bytes, size_t size,
              int64_t pts, int64_t duration)
{
    int rc;
    if (video && writer->encoder) {
        rc = av_frame_make_writable(writer->frame);
        if (rc < 0)
            return rc;
        unpack_video(writer->frame, bytes);
        writer->frame->pts = pts;
        writer->frame->duration = duration;
        if (writer->filter)
            rc = filter_video(writer, writer->frame);
        else
            rc = encode(writer->mux, writer->video, writer->encoder, writer->frame, writer->packet);
    } else if (!video && writer->audio_encoder) {
        rc = encode_pcm(writer, bytes, size, pts);
    } else {
        // Give the muxer its own copy because the recorder reuses the queue slot after this call.
        rc = av_new_packet(writer->packet, (int)size);
        if (rc < 0)
            return rc;
        memcpy(writer->packet->data, bytes, size);
        writer->packet->pts = writer->packet->dts = pts;
        writer->packet->duration = duration;
        writer->packet->flags = AV_PKT_FLAG_KEY;
        const AVStream *stream = video ? writer->video : writer->audio;
        rc = write_packet(writer->mux, stream, writer->packet, ns_time_base);
    }
    rc = checkpoint(writer, rc);
    writer->has_samples |= rc >= 0;
    return rc;
}

int capture_writer_close(struct capture_writer *writer, int error, bool *finalized, uint64_t *bytes)
{
    if (error >= 0 && writer->has_samples) {
        if (writer->filter)
            error = filter_video(writer, NULL);
        if (error >= 0 && writer->encoder)
            error = encode(writer->mux, writer->video, writer->encoder, NULL, writer->packet);
        if (error >= 0 && writer->audio_encoder)
            error = encode(writer->mux, writer->audio, writer->audio_encoder, NULL, writer->packet);
        // Keep the last complete append before attempting to flush the container trailer.
        error = checkpoint(writer, error);
        if (error >= 0)
            error = checkpoint(writer, av_write_trailer(writer->mux));
    }
    if (writer->fd >= 0 && (!writer->has_samples || writer->checkpoint == 0)) {
        // A capture stopped before any usable header or media should only leave its report.
        if (unlink(writer->path) == 0)
            writer->offset = 0;
        else if (error >= 0)
            error = AVERROR(errno);
    }
    if (writer->fd >= 0) {
        // A failed append may leave part of a cluster or fragment. Keep the preceding prefix.
        if (error < 0 && writer->offset && writer->offset != writer->checkpoint) {
            if (ftruncate(writer->fd, (off_t)writer->checkpoint) == 0)
                writer->offset = writer->checkpoint;
            else
                perror("Truncate incomplete container append");
        }
        if (fsync(writer->fd) != 0 && error >= 0)
            error = AVERROR(errno);
        if (close(writer->fd) != 0 && error >= 0)
            error = AVERROR(errno);
    }
    *finalized = error >= 0 && writer->has_samples;
    *bytes = writer->offset;
    if (writer->mux && writer->mux->pb) {
        av_freep(&writer->mux->pb->buffer);
        avio_context_free(&writer->mux->pb);
    }
    avcodec_free_context(&writer->encoder);
    avcodec_free_context(&writer->audio_encoder);
    avfilter_graph_free(&writer->filter);
    avformat_free_context(writer->mux);
    av_frame_free(&writer->frame);
    av_frame_free(&writer->filtered);
    av_frame_free(&writer->pcm);
    av_packet_free(&writer->packet);
    av_free(writer->path);
    free(writer);
    return error;
}

int capture_writer_publish(const char *directory, enum capture_profile profile)
{
    char *partial = av_asprintf("%s/%s", directory, capture_media_name(profile, false));
    char *final = av_asprintf("%s/%s", directory, capture_media_name(profile, true));
    int rc = AVERROR(ENOMEM);
    if (partial && final) {
        rc = link(partial, final) == 0 ? 0 : AVERROR(errno);
        if (rc >= 0 && unlink(partial) != 0)
            perror(partial);
    }
    av_free(partial);
    av_free(final);
    return rc;
}
