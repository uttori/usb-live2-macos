/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CX231XX_RAW_H
#define CX231XX_RAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cx231xx-video.h"
#include "cx231xx-view.h"

enum {
    // One interleaved stereo sample: two signed 16-bit little-endian channel values.
    PCM_FRAME_BYTES = 4,
    PCM_BYTES_PER_SECOND = 48000 * PCM_FRAME_BYTES,
    // Diagnostic dumps retain every raw frame in RAM; five seconds already needs about 119 MiB.
    RAW_MAX_SECONDS = 5,
};

/** Buffers and parser for a bounded diagnostic dump. USB callbacks only write to memory. */
struct raw_dump;
struct recorder;
struct capture_stats;
struct capture_monitor;

struct raw_dump *raw_dump_create(unsigned seconds);
/** Borrow the optional recorder and viewers until this parser is destroyed. */
struct raw_dump *raw_dump_create_live(struct recorder *recorder, struct live_view *const views[VIEW_COUNT]);
/** Borrow an in-process preview sink for the lifetime of this live parser. */
void raw_dump_attach_monitor(struct raw_dump *dump, const struct capture_monitor *monitor);
/** Tap a live preview for recording. start_ns is relative to the USB stream's origin. */
void raw_dump_start_recording(struct raw_dump *dump, struct recorder *recorder, uint64_t start_ns);
/** Flush complete PCM to the recorder and detach it without disturbing preview packet assembly. */
void raw_dump_stop_recording(struct raw_dump *dump, struct capture_stats *stats);
void raw_dump_destroy(struct raw_dump *dump);
bool raw_dump_live_failed(const struct raw_dump *dump);
/** A view-only session can stop when its last output closes. */
bool raw_dump_view_closed(const struct raw_dump *dump);
/** Queue the final complete PCM frames and collect parser counters after USB shutdown. */
void raw_dump_finish_live(struct raw_dump *dump, struct capture_stats *stats);

/** Set the shared monotonic origin in nanoseconds before feeding either endpoint. */
void raw_dump_set_origin(struct raw_dump *dump, uint64_t origin_ns);

/**
 * Feed a successful packet in endpoint order. callback_ns is the host observation time
 * relative to the origin, shared by all packets in that completion. False stops acquisition
 * because the buffer or writer rejected a unit, or the live parser found a discontinuity.
 */
bool raw_dump_feed(struct raw_dump *dump, bool video, const uint8_t *bytes, size_t size,
                   uint64_t callback_ns);

/** Write raw samples and timing after shutdown. */
bool raw_dump_write(const struct raw_dump *dump, const char *directory, bool transport_ok);

#endif
