/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CX231XX_RECORD_H
#define CX231XX_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cx231xx-writer.h"

struct recorder;

/** Producer-side measurements. USB arrays are ordered audio, then video. */
struct capture_stats {
    uint64_t origin_ns;
    double elapsed;
    uint64_t bytes[2];
    uint64_t packet_errors[2];
    uint64_t transfer_errors[2];
    uint64_t submit_errors[2];
    uint64_t rejected_frames;
    unsigned trailing_pcm_bytes;
    // H/V lock bits from the decoder, at acquisition and the last successful check.
    uint32_t initial_video_lock, last_video_lock;
    int usb_status[2];
    int signal;
};

// 1,024 stereo s16le samples per block: 21.33 ms at 48 kHz and one native AAC frame.
enum { RECORD_PCM_BYTES = 4096 };

/** Create a private output directory and reserve the report before any media is written. */
struct recorder *recorder_open(const char *directory, bool svideo, enum capture_profile profile);

/** Copy one unit from the USB thread. Never waits for queue space or encoding. */
bool recorder_push(struct recorder *rec, bool video, const uint8_t *bytes, size_t size,
                   uint64_t callback_ns);

/** The current writer or queue error, in FFmpeg's negative error-code form. */
int recorder_error(struct recorder *rec);

/** Drain, finalize, and report after the producer has stopped. Consumes rec. */
bool recorder_finish(struct recorder *rec, const char *reason, bool complete,
                     const struct capture_stats *stats);

#endif
