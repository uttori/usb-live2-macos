// SPDX-License-Identifier: GPL-2.0-or-later
#define _DARWIN_C_SOURCE
#include "cx231xx-raw.h"
#include "cx231xx-monitor.h"
#include "cx231xx-record.h"

#include <assert.h>
#include <errno.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** A stop before acquisition leaves a report without advertising an empty media file. */
static void check_empty_record(const char *parent)
{
    char directory[128], path[160];
    snprintf(directory, sizeof directory, "%s/empty", parent);
    struct recorder *recorder = recorder_open(directory, false, CAPTURE_FFV1);
    assert(recorder);
    const struct capture_stats stats = {0};
    assert(!recorder_finish(recorder, "no_video_lock", false, &stats));
    snprintf(path, sizeof path, "%s/capture.partial.mkv", directory);
    assert(access(path, F_OK) == -1 && errno == ENOENT);
    snprintf(path, sizeof path, "%s/capture.json", directory);
    FILE *report = fopen(path, "rb");
    assert(report);
    char json[4097];
    size_t length = fread(json, 1, sizeof json - 1, report);
    json[length] = 0;
    assert(strstr(json, "\"stop_reason\": \"no_video_lock\"") &&
           strstr(json, "\"matroska\": null") && strstr(json, "\"finalized\": false"));
    assert(fclose(report) == 0 && unlink(path) == 0 && rmdir(directory) == 0);
}

/** A full queue must fail immediately while the writer drains the accepted PCM prefix. */
static void check_record_queue(const char *parent)
{
    char directory[128], path[160];
    snprintf(directory, sizeof directory, "%s/queue", parent);
    struct recorder *recorder = recorder_open(directory, false, CAPTURE_FFV1);
    assert(recorder);
    const uint8_t pcm[RECORD_PCM_BYTES] = {0};
    unsigned blocks = 0;
    while (blocks < 100000 &&
           recorder_push(recorder, false, pcm, sizeof pcm, ++blocks * UINT64_C(21333333))) {
    }
    assert(recorder_error(recorder) == -ENOBUFS);
    const struct capture_stats stats = {0};
    assert(!recorder_finish(recorder, "usb_error", false, &stats));
    snprintf(path, sizeof path, "%s/capture.json", directory);
    FILE *report = fopen(path, "rb");
    assert(report);
    char json[4097];
    size_t length = fread(json, 1, sizeof json - 1, report);
    json[length] = 0;
    assert(strstr(json, "\"stop_reason\": \"queue_full\"") &&
           strstr(json, "\"finalized\": true") && strstr(json, "\"audio_queue_high_water\": 96"));
    assert(fclose(report) == 0 && unlink(path) == 0);
    snprintf(path, sizeof path, "%s/capture.partial.mkv", directory);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

/** The uncompressed MOV must carry the exact woven YUYV and stereo PCM packets. */
static void check_raw_record(const char *directory)
{
    static uint8_t frame[NTSC_FRAME_BYTES];
    for (size_t i = 0; i < sizeof frame; i += 4) {
        // Use different luma on the two fields and distinct U/V bytes to detect rearrangement.
        frame[i] = frame[i + 2] = (i / NTSC_LINE_BYTES) % 2 ? 180 : 40;
        frame[i + 1] = 70;
        frame[i + 3] = 150;
    }
    const uint8_t pcm[] = {1, 2, 3, 4, 5, 6, 7, 8};
    struct capture_writer *writer = NULL;
    assert(capture_writer_open(&writer, directory, CAPTURE_RAW) == 0);
    assert(capture_writer_write(writer, true, frame, sizeof frame, 0, 33366667) == 0);
    assert(capture_writer_write(writer, false, pcm, sizeof pcm, 5000000, 41667) == 0);
    bool finalized;
    uint64_t bytes;
    assert(capture_writer_close(writer, 0, &finalized, &bytes) == 0 && finalized);
    assert(capture_writer_publish(directory, CAPTURE_RAW) == 0);

    char path[128];
    snprintf(path, sizeof path, "%s/capture.mov", directory);
    AVFormatContext *input = NULL;
    assert(avformat_open_input(&input, path, NULL, NULL) == 0);
    assert(avformat_find_stream_info(input, NULL) >= 0);
    assert(input->nb_streams == 2);
    const AVCodecParameters *video = input->streams[0]->codecpar;
    assert(video->codec_id == AV_CODEC_ID_RAWVIDEO && video->format == AV_PIX_FMT_YUYV422);
    assert(video->field_order == AV_FIELD_TT);
    const AVCodecParameters *audio = input->streams[1]->codecpar;
    assert(audio->codec_id == AV_CODEC_ID_PCM_S16LE && audio->sample_rate == 48000 &&
           audio->ch_layout.nb_channels == 2);
    AVPacket *packet = av_packet_alloc();
    assert(packet);
    assert(av_read_frame(input, packet) == 0 && packet->stream_index == 0);
    assert(packet->pts == 0 && packet->size == sizeof frame &&
           memcmp(packet->data, frame, sizeof frame) == 0);
    av_packet_unref(packet);
    assert(av_read_frame(input, packet) == 0 && packet->stream_index == 1);
    assert(av_compare_ts(packet->pts, input->streams[1]->time_base, 5, (AVRational){1, 1000}) == 0 &&
           packet->size == sizeof pcm &&
           memcmp(packet->data, pcm, sizeof pcm) == 0);
    assert(av_read_frame(input, packet) == AVERROR_EOF);
    av_packet_free(&packet);
    avformat_close_input(&input);
    assert(unlink(path) == 0);
}

static void marker(struct raw_dump *dump, uint8_t code, size_t split)
{
    const uint8_t bytes[] = {0xff, 0, 0, code};
    assert(raw_dump_feed(dump, true, bytes, split, code));
    assert(raw_dump_feed(dump, true, NULL, 0, code));
    assert(raw_dump_feed(dump, true, bytes + split, sizeof bytes - split, code + 1));
}

/** Generate field-specific chroma and row-specific luma to catch row and byte-order mistakes. */
static void field(struct raw_dump *dump, unsigned parity, unsigned count, bool eav,
                  unsigned damaged_row)
{
    uint8_t wire[NTSC_LINE_BYTES];
    for (unsigned row = 0; row < count; ++row) {
        marker(dump, parity ? 0xc0 : 0x80, row % 5);
        for (size_t i = 0; i < sizeof wire; i += 4) {
            wire[i] = (uint8_t)(70 + parity);
            wire[i + 1] = 16 + row % 200;
            wire[i + 2] = (uint8_t)(170 + parity);
            wire[i + 3] = 17 + row % 200;
        }
        // Use odd payload fragments and a false marker prefix at the end of a packet.
        memcpy(wire + 4, (uint8_t[]){0xff, 0, 0, 0xc8}, 4);
        assert(raw_dump_feed(dump, true, wire, 5, 0));
        assert(raw_dump_feed(dump, true, wire + 5, sizeof wire - 5 - (row == damaged_row), 0));
        if (eav) {
            marker(dump, parity ? 0xda : 0x9d, (row + 2) % 5);
            assert(raw_dump_feed(dump, true, (uint8_t[]){9, 8, 7}, 3, 0));
        }
    }
}

static void check_video(const char *directory, unsigned frames)
{
    char path[128];
    snprintf(path, sizeof path, "%s/video.yuyv", directory);
    FILE *file = fopen(path, "rb");
    assert(file);
    uint8_t line[NTSC_LINE_BYTES];
    for (unsigned f = 0; f < frames; ++f) {
        for (unsigned row = 0; row < NTSC_FIELD_LINES * 2; ++row) {
            assert(fread(line, 1, sizeof line, file) == sizeof line);
            for (size_t i = 0; i < sizeof line; i += 4) {
                uint8_t expected[] = {16 + row / 2 % 200, 70 + row % 2,
                                      17 + row / 2 % 200, 170 + row % 2};
                if (i == 4)
                    memcpy(expected, (uint8_t[]){0, 0xff, 0xc8, 0}, 4);
                assert(memcmp(line + i, expected, sizeof expected) == 0);
            }
        }
    }
    assert(fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0 && unlink(path) == 0);
}

static void check_audio(const char *directory, const uint8_t *expected, size_t size)
{
    char path[128];
    snprintf(path, sizeof path, "%s/audio.s16le", directory);
    FILE *file = fopen(path, "rb");
    assert(file);
    for (size_t i = 0; i < size; ++i)
        assert(fgetc(file) == expected[i]);
    assert(fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0 && unlink(path) == 0);
}

static void check_timing(const char *directory, bool complete, bool split_audio)
{
    char path[128];
    snprintf(path, sizeof path, "%s/timing.json", directory);
    FILE *file = fopen(path, "rb");
    assert(file);
    char data[4096];
    size_t size = fread(data, 1, sizeof data - 1, file);
    assert(feof(file) && !ferror(file));
    data[size] = 0;
    assert(strstr(data, complete ? "\"complete\": true" : "\"complete\": false"));
    assert(strstr(data, "\"origin_ns\": 1000000"));
    assert(strstr(data, "\"frame\": 0, \"field1_ns\": 129, \"field2_ns\": 193"));
    if (split_audio) {
        assert(strstr(data, "\"first_sample\": 0, \"samples\": 1, \"callback_ns\": 20"));
        assert(strstr(data, "\"first_sample\": 1, \"samples\": 1, \"callback_ns\": 30"));
    } else {
        assert(strstr(data, "\"first_sample\": 0, \"samples\": 2, \"callback_ns\": 22"));
    }
    assert(fclose(file) == 0 && unlink(path) == 0);
}

/** Count the bytes actually passed to a native preview with no OBS output. */
static void count_monitor_pcm(void *context, const uint8_t *pcm, size_t size)
{
    size_t *bytes = context;
    assert(pcm[0] == 1 && size % PCM_FRAME_BYTES == 0);
    *bytes += size;
}

static void ignore_monitor_video(void *context, const uint8_t *frame)
{
    (void)context;
    (void)frame;
}

/** Exercise preview-only PCM and attaching a writer between stereo frames without leaks or duplication. */
static void check_monitor_recording(const char *parent)
{
    struct live_view *views[VIEW_COUNT] = {0};
    struct raw_dump *dump = raw_dump_create_live(NULL, views);
    assert(dump);
    size_t preview_bytes = 0;
    const struct capture_monitor monitor = {
        .context = &preview_bytes, .video = ignore_monitor_video, .audio = count_monitor_pcm,
    };
    raw_dump_attach_monitor(dump, &monitor);
    uint8_t audio[RECORD_PCM_BYTES] = {1};
    assert(raw_dump_feed(dump, false, audio, sizeof audio - 1, 21));
    assert(raw_dump_feed(dump, false, audio + sizeof audio - 1, 1, 22));
    assert(preview_bytes == sizeof audio);

    char directory[128], path[160];
    snprintf(directory, sizeof directory, "%s/monitor", parent);
    struct recorder *recorder = recorder_open(directory, true, CAPTURE_FFV1);
    assert(recorder);
    // Discard the old samples and the first stereo sample split at the start boundary.
    assert(raw_dump_feed(dump, false, audio, 2, 110));
    raw_dump_start_recording(dump, recorder, 120);
    const uint8_t pcm[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    assert(raw_dump_feed(dump, false, pcm, sizeof pcm, 24000000));
    field(dump, 1, 3, false, 999);
    field(dump, 0, 240, false, 999);
    field(dump, 1, 240, false, 999);
    marker(dump, 0x80, 1);
    struct capture_stats stats = {.origin_ns = 120};
    raw_dump_stop_recording(dump, &stats);
    assert(stats.trailing_pcm_bytes == 1);
    assert(recorder_finish(recorder, "user_stopped", true, &stats));

    // Detaching flushes the recording but leaves bytes for the preview's next full PCM block.
    assert(raw_dump_feed(dump, false, audio, sizeof audio - sizeof pcm - 2, 45000000));
    assert(preview_bytes == sizeof audio * 2);
    raw_dump_destroy(dump);

    snprintf(path, sizeof path, "%s/capture.mkv", directory);
    AVFormatContext *input = NULL;
    assert(avformat_open_input(&input, path, NULL, NULL) == 0 && input->nb_streams == 2);
    AVPacket *packet = av_packet_alloc();
    assert(packet);
    bool saw_video = false, saw_audio = false;
    while (av_read_frame(input, packet) == 0) {
        enum AVCodecID codec = input->streams[packet->stream_index]->codecpar->codec_id;
        if (codec == AV_CODEC_ID_PCM_S16LE) {
            assert(!saw_audio && packet->size == 8 && memcmp(packet->data, pcm + 2, 8) == 0);
            saw_audio = true;
        } else if (codec == AV_CODEC_ID_FFV1) {
            assert(!saw_video);
            saw_video = true;
        }
        av_packet_unref(packet);
    }
    assert(saw_video && saw_audio);
    av_packet_free(&packet);
    avformat_close_input(&input);
    assert(unlink(path) == 0);
    snprintf(path, sizeof path, "%s/capture.json", directory);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
}

int main(void)
{
    char parent[] = "/tmp/usblive2-raw-test.XXXXXX";
    assert(mkdtemp(parent));
    char directory[128];
    snprintf(directory, sizeof directory, "%s/sample", parent);
    struct raw_dump *dump = raw_dump_create(1);
    assert(dump);
    raw_dump_set_origin(dump, 1000000);

    field(dump, 1, 3, false, 999); // Partial first field is ignored.
    field(dump, 0, 243, false, 999);
    field(dump, 1, 244, false, 999);
    marker(dump, 0xab, 1); // Field-1 blanking can close a SAV-only line and the frame.
    field(dump, 0, 240, true, 999);
    marker(dump, 0xec, 2);
    field(dump, 1, 240, true, 999);
    marker(dump, 0x20, 3); // The bridge can use a raw VBI marker at a field transition.
    const uint8_t pcm[] = {1, 2, 3, 4, 0xff, 0x7f, 0, 0x80, 9, 10, 11};
    for (size_t i = 0; i < sizeof pcm; ++i)
        assert(raw_dump_feed(dump, false, pcm + i, 1, i < 6 ? 20 : 30));
    assert(raw_dump_write(dump, directory, true));
    assert(!raw_dump_write(dump, directory, true)); // Existing outputs must survive a second write.
    check_video(directory, 2);
    check_audio(directory, pcm, 8);
    check_timing(directory, true, true);
    assert(rmdir(directory) == 0);
    assert(!raw_dump_write(dump, directory, false)); // Preserve raw samples on interruption.
    check_video(directory, 2);
    check_audio(directory, pcm, 8);
    check_timing(directory, false, true);
    assert(rmdir(directory) == 0);
    raw_dump_destroy(dump);

    dump = raw_dump_create(1);
    assert(dump);
    raw_dump_set_origin(dump, 1000000);
    marker(dump, 0xc0, 1);
    field(dump, 0, 240, true, 23); // Short line invalidates the pair.
    field(dump, 1, 240, true, 999);
    field(dump, 0, 240, true, 999);
    marker(dump, 0xa0, 2);
    field(dump, 0, 240, true, 999); // Repeated field without the opposite parity.
    field(dump, 1, 240, true, 999);
    field(dump, 0, 240, false, 999);
    field(dump, 1, 240, false, 999);
    marker(dump, 0x80, 3);
    assert(raw_dump_feed(dump, false, pcm, 8, 22));
    assert(!raw_dump_feed(dump, false, pcm, PCM_BYTES_PER_SECOND * 2 + 1, 23));
    assert(!raw_dump_write(dump, directory, true)); // The recovered frame is available, with an error exit.
    check_video(directory, 1);
    check_audio(directory, pcm, 8);
    check_timing(directory, false, false);
    assert(rmdir(directory) == 0);
    raw_dump_destroy(dump);
    check_empty_record(parent);
    check_record_queue(parent);
    check_raw_record(parent);
    check_monitor_recording(parent);
    assert(rmdir(parent) == 0);
}
