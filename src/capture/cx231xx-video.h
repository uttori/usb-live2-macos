// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CX231XX_VIDEO_H
#define CX231XX_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The USB decoder supplies 8-bit 4:2:2 (two bytes per pixel); retain 240 rows of each NTSC field.
enum {
    NTSC_WIDTH = 720,
    NTSC_FIELD_LINES = 240,
    NTSC_LINE_BYTES = NTSC_WIDTH * 2,
    NTSC_FRAME_BYTES = NTSC_LINE_BYTES * NTSC_FIELD_LINES * 2,
};

/** BT.656 packet parser, zero-initialized except for the required frame callback and its context. */
struct cx231xx_video {
    /** Borrows a woven YUYV pair, or NULL on a damaged pair. Returning false stops parsing. */
    bool (*frame_ready)(void *context, const uint8_t *frame, const uint64_t field_ns[2]);
    void *context;
    uint64_t field_callback_ns[2];
    uint8_t frame[NTSC_FRAME_BYTES];
    uint8_t line[NTSC_LINE_BYTES];
    unsigned prefix; // Bytes of the current SAV/EAV preamble held back from the line.
    unsigned line_size;
    unsigned lines[2]; // Active rows written for each F bit.
    unsigned field_lines; // All complete active rows seen in the current field.
    unsigned field;
    bool seen_field;
    bool collecting;
    bool active_line;
    bool blank_after_active;
    bool damaged;
};

/** Feed bytes in endpoint order. callback_ns is the host observation of this USB completion. */
bool cx231xx_video_feed(struct cx231xx_video *parser, const uint8_t *bytes, size_t size,
                        uint64_t callback_ns);

#endif
