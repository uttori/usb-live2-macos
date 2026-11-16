// SPDX-License-Identifier: GPL-2.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "cx231xx-record.h"
#include "cx231xx-writer.h"
#include "cx231xx-raw.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// About two seconds per stream: 60 NTSC frames (~40 MiB) and 96 × 1024 stereo samples.
// Reserve a 4 KiB report before media so disk-full recovery can overwrite existing storage.
enum { VIDEO_QUEUE = 60, AUDIO_QUEUE = 96, REPORT_BYTES = 4096, NS_PER_SECOND = 1000000000 };

struct record_unit {
    size_t size;
    uint64_t callback_ns;
};

/** A slot remains occupied until encoding finishes, so the producer cannot overwrite it. */
struct record_queue {
    struct record_unit units[AUDIO_QUEUE];
    uint8_t *bytes;
    size_t capacity, stride, read, count, high_water;
};

struct recorder {
    char *directory;
    bool svideo;
    enum capture_profile profile;
    int report_fd;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    struct capture_writer *writer;
    struct record_queue video, audio;
    bool stopping, finalized;
    int error;
    uint64_t video_written, audio_written, file_bytes;
    int64_t first_video_pts, first_audio_pts, video_end_pts, audio_end_pts;
    double video_max_residual_ns, audio_max_residual_ns;
    struct AVSHA *sha[2];
    uint8_t digest[2][32];
};

/** Writer-thread phase estimate in nanoseconds; index counts frames or stereo samples. */
struct live_clock {
    uint64_t index;
    double start_ns;
    double correction_ns;
    double max_residual_ns;
    int64_t first_pts;
};

/** Anchor to the first observation, then gradually follow host phase without inserting samples. */
static int64_t live_pts(struct live_clock *clock, double observed_start, uint64_t count,
                        double step_ns, int64_t *duration)
{
    if (clock->index == 0)
        clock->start_ns = fmax(0, observed_start);
    double predicted = clock->start_ns + (double)clock->index * step_ns + clock->correction_ns;
    double residual = observed_start - predicted;
    clock->max_residual_ns = fmax(clock->max_residual_ns, fabs(residual));
    // Limit correction per unit to 0.5% so callback jitter cannot reverse or collapse timestamps.
    double limit = (double)count * step_ns * 0.005;
    // A 1/128 phase gain smooths completion jitter; the bounded slew follows slow hardware drift.
    clock->correction_ns += fmax(-limit, fmin(limit, residual / 128));
    int64_t pts = llround(clock->start_ns + (double)clock->index * step_ns + clock->correction_ns);
    if (clock->index == 0)
        clock->first_pts = pts;
    clock->index += count;
    *duration = llround((double)count * step_ns);
    return pts;
}

static void *record_thread(void *opaque)
{
    struct recorder *rec = opaque;
    struct live_clock video_clock = {0}, audio_clock = {0};
    int error = 0;
    pthread_mutex_lock(&rec->lock);
    while (error >= 0) {
        while (!rec->video.count && !rec->audio.count && !rec->stopping)
            pthread_cond_wait(&rec->ready, &rec->lock);
        if (!rec->video.count && !rec->audio.count)
            break;
        bool video = rec->video.count && (!rec->audio.count ||
            rec->video.units[rec->video.read].callback_ns <= rec->audio.units[rec->audio.read].callback_ns);
        struct record_queue *queue = video ? &rec->video : &rec->audio;
        struct record_unit unit = queue->units[queue->read];
        const uint8_t *bytes = queue->bytes + queue->read * queue->stride;
        pthread_mutex_unlock(&rec->lock);

        uint64_t count = video ? 1 : unit.size / PCM_FRAME_BYTES;
        double step = video ? (double)NS_PER_SECOND * 1001.0 / 30000 : (double)NS_PER_SECOND / 48000.0;
        double observed_start = (double)unit.callback_ns;
        if (!video)
            observed_start -= (double)count * step;
        int64_t duration;
        int64_t pts = live_pts(video ? &video_clock : &audio_clock, observed_start, count, step, &duration);
        error = capture_writer_write(rec->writer, video, bytes, unit.size, pts, duration);
        // Hash the complete units submitted to the muxer before recycling the slot.
        if (error >= 0)
            av_sha_update(rec->sha[video], bytes, unit.size);

        pthread_mutex_lock(&rec->lock);
        queue->read = (queue->read + 1) % queue->capacity;
        --queue->count;
        if (error < 0) {
            // A writer failure supersedes backpressure because it can prevent finalization.
            rec->error = error;
        } else if (video) {
            ++rec->video_written;
            rec->video_end_pts = pts + duration;
        } else {
            rec->audio_written += count;
            rec->audio_end_pts = pts + duration;
        }
    }
    pthread_mutex_unlock(&rec->lock);

    error = capture_writer_close(rec->writer, error, &rec->finalized, &rec->file_bytes);
    rec->first_video_pts = video_clock.first_pts;
    rec->first_audio_pts = audio_clock.first_pts;
    rec->video_max_residual_ns = video_clock.max_residual_ns;
    rec->audio_max_residual_ns = audio_clock.max_residual_ns;
    for (size_t i = 0; i < 2; ++i)
        av_sha_final(rec->sha[i], rec->digest[i]);
    pthread_mutex_lock(&rec->lock);
    if (error < 0)
        rec->error = error;
    pthread_mutex_unlock(&rec->lock);
    return NULL;
}

static bool report_write(int fd, const char *report)
{
    char page[REPORT_BYTES];
    size_t length = strlen(report);
    if (length > sizeof page)
        return false;
    memcpy(page, report, length);
    memset(page + length, ' ', sizeof page - length);
    // Overwrite a reserved page, including when media exhausted the remaining free space.
    size_t offset = 0;
    while (offset < sizeof page) {
        ssize_t written = pwrite(fd, page + offset, sizeof page - offset, (off_t)offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written == 0)
            errno = EIO;
        if (written <= 0)
            return false;
        offset += (size_t)written;
    }
    return fsync(fd) == 0;
}

static int open_report(const char *directory)
{
    size_t size = strlen(directory) + sizeof "/capture.json";
    char *path = malloc(size);
    if (!path) {
        errno = ENOMEM;
        return -1;
    }
    snprintf(path, size, "%s/capture.json", directory);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    free(path);
    return fd;
}

struct recorder *recorder_open(const char *directory, bool svideo, enum capture_profile profile)
{
    if (mkdir(directory, 0700) != 0) {
        perror(directory);
        return NULL;
    }
    struct recorder *rec = calloc(1, sizeof *rec);
    if (!rec) {
        perror("Allocate recorder");
        return NULL;
    }
    // The app prepares and finalizes the recorder on different queues from the caller.
    rec->directory = strdup(directory);
    if (!rec->directory) {
        perror("Copy recording path");
        free(rec);
        return NULL;
    }
    rec->svideo = svideo;
    rec->profile = profile;
    rec->report_fd = open_report(directory);
    int error = 0;
    if (rec->report_fd < 0 || !report_write(rec->report_fd, "{\"complete\":false,\"stop_reason\":\"in_progress\"}\n"))
        error = AVERROR(errno);
    rec->video.capacity = VIDEO_QUEUE;
    rec->video.stride = NTSC_FRAME_BYTES;
    rec->audio.capacity = AUDIO_QUEUE;
    rec->audio.stride = RECORD_PCM_BYTES;
    if (error >= 0) {
        rec->video.bytes = malloc(VIDEO_QUEUE * NTSC_FRAME_BYTES);
        rec->audio.bytes = malloc(AUDIO_QUEUE * RECORD_PCM_BYTES);
        rec->sha[0] = av_sha_alloc();
        rec->sha[1] = av_sha_alloc();
        if (!rec->video.bytes || !rec->audio.bytes || !rec->sha[0] || !rec->sha[1])
            error = AVERROR(ENOMEM);
        else {
            av_sha_init(rec->sha[0], 256);
            av_sha_init(rec->sha[1], 256);
        }
    }
    bool mutex = false, cond = false;
    if (error >= 0) {
        error = -pthread_mutex_init(&rec->lock, NULL);
        mutex = error == 0;
    }
    if (error >= 0) {
        error = -pthread_cond_init(&rec->ready, NULL);
        cond = error == 0;
    }
    if (error >= 0)
        error = capture_writer_open(&rec->writer, directory, profile);
    if (error >= 0)
        error = -pthread_create(&rec->thread, NULL, record_thread, rec);
    if (error >= 0)
        return rec;

    fprintf(stderr, "Recording setup: %s\n", av_err2str(error));
    if (rec->writer)
        capture_writer_close(rec->writer, error, &rec->finalized, &rec->file_bytes);
    const char *reason = error == AVERROR(ENOSPC) ? "disk_full" : "writer_setup_failed";
    char report[160];
    snprintf(report, sizeof report, "{\"complete\":false,\"stop_reason\":\"%s\",\"writer_error\":%d}\n",
             reason, error);
    if (rec->report_fd < 0 || !report_write(rec->report_fd, report))
        fputs(report, stderr);
    if (rec->report_fd >= 0)
        close(rec->report_fd);
    if (cond)
        pthread_cond_destroy(&rec->ready);
    if (mutex)
        pthread_mutex_destroy(&rec->lock);
    free(rec->audio.bytes);
    free(rec->video.bytes);
    av_free(rec->sha[0]);
    av_free(rec->sha[1]);
    free(rec->directory);
    free(rec);
    return NULL;
}

bool recorder_push(struct recorder *rec, bool video, const uint8_t *bytes, size_t size,
                   uint64_t callback_ns)
{
    struct record_queue *queue = video ? &rec->video : &rec->audio;
    pthread_mutex_lock(&rec->lock);
    bool ok = rec->error == 0;
    if (ok && (size == 0 || size > queue->stride ||
               (video && size != queue->stride) || (!video && size % PCM_FRAME_BYTES))) {
        rec->error = AVERROR(EINVAL);
        ok = false;
    } else if (ok && queue->count == queue->capacity) {
        rec->error = AVERROR(ENOBUFS);
        ok = false;
    }
    if (ok) {
        size_t index = (queue->read + queue->count) % queue->capacity;
        memcpy(queue->bytes + index * queue->stride, bytes, size);
        queue->units[index] = (struct record_unit){size, callback_ns};
        if (++queue->count > queue->high_water)
            queue->high_water = queue->count;
        pthread_cond_signal(&rec->ready);
    }
    pthread_mutex_unlock(&rec->lock);
    return ok;
}

int recorder_error(struct recorder *rec)
{
    pthread_mutex_lock(&rec->lock);
    int error = rec->error;
    pthread_mutex_unlock(&rec->lock);
    return error;
}

bool recorder_finish(struct recorder *rec, const char *reason, bool complete,
                     const struct capture_stats *stats)
{
    pthread_mutex_lock(&rec->lock);
    rec->stopping = true;
    pthread_cond_signal(&rec->ready);
    pthread_mutex_unlock(&rec->lock);
    pthread_join(rec->thread, NULL);

    if (rec->error == AVERROR(ENOSPC))
        reason = "disk_full";
    else if (rec->error == AVERROR(ENOBUFS))
        reason = "queue_full";
    else if (rec->error < 0)
        reason = "writer_error";
    else if (complete && (!rec->video_written || !rec->audio_written))
        reason = "missing_samples";
    complete = complete && rec->error == 0 && rec->finalized &&
               rec->video_written && rec->audio_written;
    if (complete) {
        rec->error = capture_writer_publish(rec->directory, rec->profile);
        if (rec->error < 0) {
            reason = rec->error == AVERROR(ENOSPC) ? "disk_full" : "publish_failed";
            complete = false;
        }
    }
    char hashes[2][65];
    for (size_t i = 0; i < 2; ++i)
        for (size_t byte = 0; byte < sizeof rec->digest[i]; ++byte)
            snprintf(hashes[i] + 2 * byte, 3, "%02x", rec->digest[i][byte]);
    char media[40] = "null";
    if (rec->file_bytes)
        snprintf(media, sizeof media, "\"%s\"", capture_media_name(rec->profile, complete));
    static const char *const video_formats[] = {
        "FFV1 v3, 720x480, yuv422p, top-field-first, 30000/1001",
        "ProRes 422 HQ, 720x480, yuv422p10le, top-field-first, 30000/1001",
        "H.264, 720x480, yuv420p, progressive, 60000/1001",
        "H.264 VideoToolbox, 720x480, yuv420p, progressive, 60000/1001",
        "Uncompressed, 720x480, yuyv422, top-field-first, 30000/1001",
    };
    bool aac = rec->profile == CAPTURE_H264 || rec->profile == CAPTURE_H264_HW;
    const char *audio_format = aac ? "AAC, stereo, 48000 Hz" : "pcm_s16le, stereo, 48000 Hz";
    char report[REPORT_BYTES];
    snprintf(report, sizeof report,
             "{\n  \"complete\": %s,\n  \"stop_reason\": \"%s\",\n"
             "  \"media\": %s,\n  \"matroska\": %s,\n  \"profile\": \"%s\",\n"
             "  \"finalized\": %s,\n  \"writer_error\": %d,\n"
             "  \"signal\": %d,\n"
             "  \"file_bytes\": %" PRIu64 ",\n  \"input\": \"%s\",\n"
             "  \"standard\": \"NTSC-M\",\n  \"video\": \"%s\",\n  \"audio\": \"%s\",\n"
             "  \"clock\": \"CLOCK_MONOTONIC_RAW\",\n  \"origin_ns\": %" PRIu64 ",\n"
             "  \"elapsed_seconds\": %.6f,\n  \"first_video_pts_ns\": %" PRId64 ",\n"
             "  \"first_audio_pts_ns\": %" PRId64 ",\n"
             "  \"submitted_video_end_pts_ns\": %" PRId64 ",\n"
             "  \"submitted_audio_end_pts_ns\": %" PRId64 ",\n"
             "  \"video_max_host_residual_ns\": %.0f,\n  \"audio_max_host_residual_ns\": %.0f,\n"
             "  \"video_frames_submitted\": %" PRIu64 ",\n  \"audio_samples_submitted\": %" PRIu64 ",\n"
             "  \"submitted_yuyv_sha256\": \"%s\",\n  \"submitted_pcm_sha256\": \"%s\",\n"
             "  \"video_queue_high_water\": %zu,\n  \"audio_queue_high_water\": %zu,\n"
             "  \"initial_video_lock\": %" PRIu32 ",\n  \"last_video_lock\": %" PRIu32 ",\n"
             "  \"rejected_video_frames\": %" PRIu64 ",\n  \"trailing_pcm_bytes\": %u,\n"
             "  \"usb_bytes_audio_video\": [%" PRIu64 ", %" PRIu64 "],\n"
             "  \"packet_errors_audio_video\": [%" PRIu64 ", %" PRIu64 "],\n"
             "  \"transfer_errors_audio_video\": [%" PRIu64 ", %" PRIu64 "],\n"
             "  \"submit_errors_audio_video\": [%" PRIu64 ", %" PRIu64 "],\n"
             "  \"first_usb_status_audio_video\": [%d, %d]\n}\n",
             complete ? "true" : "false", reason, media,
             rec->profile == CAPTURE_FFV1 ? media : "null", capture_profile_name(rec->profile),
             rec->finalized ? "true" : "false", rec->error, stats->signal, rec->file_bytes,
             rec->svideo ? "svideo" : "composite", video_formats[rec->profile], audio_format,
             stats->origin_ns, stats->elapsed,
             rec->first_video_pts, rec->first_audio_pts, rec->video_end_pts, rec->audio_end_pts,
             rec->video_max_residual_ns, rec->audio_max_residual_ns,
             rec->video_written, rec->audio_written, hashes[1], hashes[0],
             rec->video.high_water, rec->audio.high_water,
             stats->initial_video_lock, stats->last_video_lock,
             stats->rejected_frames, stats->trailing_pcm_bytes, stats->bytes[0], stats->bytes[1],
             stats->packet_errors[0], stats->packet_errors[1], stats->transfer_errors[0],
             stats->transfer_errors[1], stats->submit_errors[0], stats->submit_errors[1],
             stats->usb_status[0], stats->usb_status[1]);
    bool saved = report_write(rec->report_fd, report);
    if (close(rec->report_fd) != 0)
        saved = false;
    if (!saved) {
        fputs("Could not save capture.json. Final report follows.\n", stderr);
        fputs(report, stderr);
    }
    if (rec->error < 0)
        fprintf(stderr, "Recording stopped (%s): %s\n", reason, av_err2str(rec->error));
    printf("  Recording %s: %s\n", reason, rec->directory);
    pthread_cond_destroy(&rec->ready);
    pthread_mutex_destroy(&rec->lock);
    free(rec->audio.bytes);
    free(rec->video.bytes);
    av_free(rec->sha[0]);
    av_free(rec->sha[1]);
    free(rec->directory);
    free(rec);
    return complete && saved;
}
