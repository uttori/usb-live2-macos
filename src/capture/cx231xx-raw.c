// SPDX-License-Identifier: GPL-2.0-or-later
/* Wire format and field layout follow Linux v6.12 cx231xx-video.c and cx231xx-audio.c.
 * Copyright (C) 2008 Srinivasa Deevi, Conexant.
 */
#define _POSIX_C_SOURCE 200809L
#include "cx231xx-raw.h"
#include "cx231xx-monitor.h"
#include "cx231xx-record.h"
#include "cx231xx-view.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** Exclusive end sample index for the PCM frames observed in one completion. */
struct audio_observation {
    size_t end_sample;
    uint64_t callback_ns;
};

struct raw_dump {
    struct recorder *recorder;
    const struct capture_monitor *monitor;
    struct live_view *views[VIEW_COUNT];
    bool live;
    uint8_t pcm[RECORD_PCM_BYTES];
    size_t pcm_size;
    uint64_t pcm_callback_ns;
    uint64_t recording_start_ns;
    /** Bytes already buffered when recording started, rounded up to a whole stereo sample. */
    size_t recording_pcm_skip;
    uint8_t *video;
    uint8_t *audio;
    size_t video_capacity;
    size_t audio_capacity;
    size_t frames;
    size_t audio_size;
    size_t rejected;
    uint64_t origin_ns;
    uint64_t (*video_times)[2];
    struct audio_observation *audio_times;
    size_t audio_observations;
    size_t observation_capacity;
    struct cx231xx_video parser;
};

static bool dump_frame(void *context, const uint8_t *frame, const uint64_t field_ns[2])
{
    struct raw_dump *dump = context;
    if (!frame) {
        ++dump->rejected;
        // A live recording must stop at a discontinuity instead of closing the gap.
        return !dump->live;
    }
    if (dump->live) {
        // The first field can predate recording when the button was pressed mid-frame.
        uint64_t timestamp = field_ns[0];
        if (dump->recorder && timestamp >= dump->recording_start_ns &&
            !recorder_push(dump->recorder, true, frame, NTSC_FRAME_BYTES,
                           timestamp - dump->recording_start_ns) && !dump->monitor)
            return false;
        for (size_t i = 0; i < VIEW_COUNT; ++i)
            live_view_offer(dump->views[i], frame);
        if (dump->monitor)
            dump->monitor->video(dump->monitor->context, frame);
        return true;
    }
    if (dump->frames == dump->video_capacity)
        return false;
    memcpy(dump->video_times[dump->frames], field_ns, sizeof dump->video_times[0]);
    memcpy(dump->video + dump->frames++ * NTSC_FRAME_BYTES, frame, NTSC_FRAME_BYTES);
    return true;
}

struct raw_dump *raw_dump_create_live(struct recorder *recorder, struct live_view *const views[VIEW_COUNT])
{
    struct raw_dump *dump = calloc(1, sizeof *dump);
    if (dump) {
        dump->parser.frame_ready = dump_frame;
        dump->parser.context = dump;
        dump->live = true;
        dump->recorder = recorder;
        memcpy(dump->views, views, sizeof dump->views);
    }
    return dump;
}

void raw_dump_attach_monitor(struct raw_dump *dump, const struct capture_monitor *monitor)
{
    dump->monitor = monitor;
}

void raw_dump_start_recording(struct raw_dump *dump, struct recorder *recorder, uint64_t start_ns)
{
    dump->recorder = recorder;
    dump->recording_start_ns = start_ns;
    // Keep the preview's buffered PCM, but exclude samples spanning the recording boundary.
    dump->recording_pcm_skip = (dump->pcm_size + PCM_FRAME_BYTES - 1) / PCM_FRAME_BYTES * PCM_FRAME_BYTES;
}

static bool push_record_pcm(struct raw_dump *dump, size_t size)
{
    size_t skip = dump->recording_pcm_skip;
    return !dump->recorder || size <= skip || dump->pcm_callback_ns < dump->recording_start_ns ||
        recorder_push(dump->recorder, false, dump->pcm + skip, size - skip,
                      dump->pcm_callback_ns - dump->recording_start_ns);
}

void raw_dump_stop_recording(struct raw_dump *dump, struct capture_stats *stats)
{
    stats->rejected_frames = dump->rejected;
    if (dump->pcm_size > dump->recording_pcm_skip)
        stats->trailing_pcm_bytes = (unsigned)(dump->pcm_size % PCM_FRAME_BYTES);
    push_record_pcm(dump, dump->pcm_size - dump->pcm_size % PCM_FRAME_BYTES);
    dump->recorder = NULL;
    dump->recording_pcm_skip = 0;
}

bool raw_dump_live_failed(const struct raw_dump *dump)
{
    return dump && !dump->monitor && dump->recorder && recorder_error(dump->recorder) < 0;
}

bool raw_dump_view_closed(const struct raw_dump *dump)
{
    if (!dump || !dump->live || dump->recorder || dump->monitor)
        return false;
    for (size_t i = 0; i < VIEW_COUNT; ++i)
        if (live_view_running(dump->views[i]))
            return false;
    return true;
}

/** Give every active output the same assembled PCM block. */
static bool push_live_pcm(struct raw_dump *dump, size_t size)
{
    // A failed app recording is detached by the capture loop. Keep feeding its preview.
    if (!push_record_pcm(dump, size) && !dump->monitor)
        return false;
    if (dump->monitor)
        dump->monitor->audio(dump->monitor->context, dump->pcm, size);
    return true;
}

void raw_dump_finish_live(struct raw_dump *dump, struct capture_stats *stats)
{
    stats->origin_ns = dump->origin_ns;
    stats->rejected_frames = dump->rejected;
    stats->trailing_pcm_bytes = (unsigned)(dump->pcm_size % PCM_FRAME_BYTES);
    size_t complete = dump->pcm_size - stats->trailing_pcm_bytes;
    if (complete)
        push_live_pcm(dump, complete);
}

struct raw_dump *raw_dump_create(unsigned seconds)
{
    if (seconds == 0 || seconds > RAW_MAX_SECONDS)
        return NULL;
    struct raw_dump *dump = calloc(1, sizeof *dump);
    if (!dump)
        return NULL;
    dump->parser.frame_ready = dump_frame;
    dump->parser.context = dump;
    // Allow for completion batches straddling the host timer and a short startup lead.
    dump->video_capacity = 30 * (seconds + 1);
    dump->audio_capacity = PCM_BYTES_PER_SECOND * (seconds + 1);
    // The USB queue completes one 32-microframe audio transfer about every 4 ms.
    dump->observation_capacity = 300 * (seconds + 1);
    dump->video = malloc(dump->video_capacity * NTSC_FRAME_BYTES);
    dump->audio = malloc(dump->audio_capacity);
    dump->video_times = malloc(dump->video_capacity * sizeof *dump->video_times);
    dump->audio_times = malloc(dump->observation_capacity * sizeof *dump->audio_times);
    if (!dump->video || !dump->audio || !dump->video_times || !dump->audio_times) {
        raw_dump_destroy(dump);
        return NULL;
    }
    return dump;
}

void raw_dump_destroy(struct raw_dump *dump)
{
    if (dump) {
        free(dump->video);
        free(dump->audio);
        free(dump->video_times);
        free(dump->audio_times);
        free(dump);
    }
}

void raw_dump_set_origin(struct raw_dump *dump, uint64_t origin_ns)
{
    dump->origin_ns = origin_ns;
}

bool raw_dump_feed(struct raw_dump *dump, bool video, const uint8_t *bytes, size_t size,
                   uint64_t callback_ns)
{
    if (size == 0)
        return true;
    if (video)
        return cx231xx_video_feed(&dump->parser, bytes, size, callback_ns);
    if (dump->live) {
        if (!dump->recorder && !dump->monitor)
            return true;
        // Group PCM into whole samples without aligning independently at USB packet boundaries.
        while (size) {
            size_t count = sizeof dump->pcm - dump->pcm_size;
            if (count > size)
                count = size;
            memcpy(dump->pcm + dump->pcm_size, bytes, count);
            dump->pcm_size += count;
            dump->pcm_callback_ns = callback_ns;
            bytes += count;
            size -= count;
            if (dump->pcm_size == sizeof dump->pcm) {
                if (!push_live_pcm(dump, dump->pcm_size))
                    return false;
                dump->pcm_size = 0;
                dump->recording_pcm_skip = 0;
            }
        }
        return true;
    }
    // Keep the partial stereo sample in the same buffer until a later packet completes it.
    if (size > dump->audio_capacity - dump->audio_size)
        return false;
    size_t complete = (dump->audio_size + size) / PCM_FRAME_BYTES;
    if (complete > dump->audio_size / PCM_FRAME_BYTES) {
        size_t index = dump->audio_observations;
        // A stereo frame split across packets belongs to the completion that finishes it.
        if (index && dump->audio_times[index - 1].callback_ns == callback_ns) {
            --index;
        } else {
            if (index == dump->observation_capacity)
                return false;
            ++dump->audio_observations;
        }
        dump->audio_times[index] = (struct audio_observation){complete, callback_ns};
    }
    memcpy(dump->audio + dump->audio_size, bytes, size);
    dump->audio_size += size;
    return true;
}

static bool write_sample(const char *directory, const char *name, const uint8_t *bytes, size_t size)
{
    size_t path_size = strlen(directory) + strlen(name) + 2;
    char *path = malloc(path_size);
    if (!path) {
        perror("Allocate sample path");
        return false;
    }
    snprintf(path, path_size, "%s/%s", directory, name);
    FILE *file = fopen(path, "wbx");
    bool ok = file != NULL;
    if (file) {
        ok = fwrite(bytes, 1, size, file) == size;
        if (fclose(file) != 0)
            ok = false;
        // A partially written file can end inside a frame. Leave only complete sample files.
        if (!ok) {
            perror(path);
            if (unlink(path) != 0)
                perror("Remove incomplete sample");
        }
    } else {
        perror(path);
    }
    free(path);
    return ok;
}

static bool write_timing(const struct raw_dump *dump, const char *directory,
                         bool video, bool audio, bool complete)
{
    size_t size = strlen(directory) + sizeof "/timing.json";
    char *path = malloc(size);
    if (!path) {
        perror("Allocate timing path");
        return false;
    }
    snprintf(path, size, "%s/timing.json", directory);
    FILE *file = fopen(path, "wx");
    bool ok = file != NULL;
    if (file) {
        fprintf(file, "{\n  \"clock\": \"CLOCK_MONOTONIC_RAW\",\n  \"origin_ns\": %" PRIu64 ",\n"
                "  \"timestamp_kind\": \"host_callback_observation\",\n  \"complete\": %s,\n"
                "  \"video_rate\": \"30000/1001\",\n  \"audio_rate_hz\": 48000,\n"
                "  \"rejected_video_frames\": %zu,\n  \"trailing_pcm_bytes\": %zu,\n",
                dump->origin_ns, complete ? "true" : "false", dump->rejected,
                dump->audio_size % PCM_FRAME_BYTES);
        fputs("  \"video_fields\": [", file);
        // Each row identifies the woven output frame and the two first-active-SAV observations.
        for (size_t i = 0; video && i < dump->frames; ++i)
            fprintf(file, "%s\n    {\"frame\": %zu, \"field1_ns\": %" PRIu64 ", \"field2_ns\": %" PRIu64 "}",
                    i ? "," : "", i, dump->video_times[i][0], dump->video_times[i][1]);
        fputs("\n  ],\n  \"audio_blocks\": [", file);
        size_t start = 0;
        for (size_t i = 0; audio && i < dump->audio_observations; ++i) {
            struct audio_observation block = dump->audio_times[i];
            fprintf(file, "%s\n    {\"first_sample\": %zu, \"samples\": %zu, \"callback_ns\": %" PRIu64 "}",
                    i ? "," : "", start, block.end_sample - start, block.callback_ns);
            start = block.end_sample;
        }
        fputs("\n  ]\n}\n", file);
        ok = !ferror(file);
        if (fclose(file) != 0)
            ok = false;
        if (!ok && unlink(path) != 0)
            perror("Remove incomplete timing file");
    }
    if (!ok)
        perror(path);
    free(path);
    return ok;
}

bool raw_dump_write(const struct raw_dump *dump, const char *directory, bool transport_ok)
{
    size_t audio_bytes = dump->audio_size - dump->audio_size % PCM_FRAME_BYTES;
    printf("  Raw dump: %zu woven frames, %zu stereo sample frames, %zu rejected video frames, "
           "%zu trailing PCM bytes\n", dump->frames, audio_bytes / PCM_FRAME_BYTES,
           dump->rejected, dump->audio_size - audio_bytes);
    if (!dump->frames && !audio_bytes) {
        fputs("No complete raw samples to write.\n", stderr);
        return false;
    }
    if (mkdir(directory, 0700) != 0) {
        perror(directory);
        return false;
    }
    bool video = dump->frames && write_sample(directory, "video.yuyv", dump->video,
                                              dump->frames * NTSC_FRAME_BYTES);
    bool audio = audio_bytes && write_sample(directory, "audio.s16le", dump->audio, audio_bytes);
    bool complete = transport_ok && video && audio && dump->rejected == 0;
    bool timed = write_timing(dump, directory, video, audio, complete);
    printf("  Raw output directory: %s\n", directory);
    return complete && timed;
}
