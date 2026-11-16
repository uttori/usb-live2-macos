// SPDX-License-Identifier: GPL-2.0-or-later
/* Wire format and field layout follow Linux v6.12 cx231xx-video.c and cx231xx-audio.c.
 * Copyright (C) 2008 Srinivasa Deevi, Conexant.
 */
#include "cx231xx-video.h"

static void line_byte(struct cx231xx_video *parser, uint8_t byte)
{
    if (!parser->active_line)
        return;
    // Saturate at one byte over width so malformed input cannot grow the counter indefinitely.
    if (parser->line_size < NTSC_LINE_BYTES)
        parser->line[parser->line_size++] = byte;
    else
        parser->line_size = NTSC_LINE_BYTES + 1;
}

/** Accept SAV-only streams and streams with EAV. In either case a marker closes the line. */
static void finish_line(struct cx231xx_video *parser)
{
    if (!parser->active_line)
        return;
    parser->active_line = false;
    unsigned field = parser->field;
    // An EAV can already carry the next line's F/V flags. The opening SAV owns this row.
    if (parser->line_size != NTSC_LINE_BYTES) {
        parser->damaged = true;
        return;
    }
    // NTSC timing programs 487 active lines per pair. A longer field signals a missed boundary.
    if (parser->field_lines <= 244)
        ++parser->field_lines;
    if (parser->field_lines > 244)
        parser->damaged = true;
    // The bridge sends 243/244 lines on the observed NTSC stream. Linux takes the first 240.
    if (parser->lines[field] >= NTSC_FIELD_LINES)
        return;
    size_t row = 2 * parser->lines[field]++ + field;
    uint8_t *dest = parser->frame + row * NTSC_LINE_BYTES;
    // Swap byte pairs after assembly so a USB split can never shift UYVY-to-YUYV conversion.
    for (size_t i = 0; i < NTSC_LINE_BYTES; i += 2) {
        dest[i] = parser->line[i + 1];
        dest[i + 1] = parser->line[i];
    }
}

static bool video_marker(struct cx231xx_video *parser, uint8_t marker, uint64_t callback_ns)
{
    finish_line(parser);
    unsigned field = (marker >> 6) & 1;
    if (!parser->seen_field || parser->field != field) {
        // Synchronize only on an observed field 2 → field 1 boundary. Discard startup fragments.
        if (field == 0 && parser->seen_field) {
            if (parser->collecting) {
                bool valid = !parser->damaged && parser->lines[0] == NTSC_FIELD_LINES &&
                             parser->lines[1] == NTSC_FIELD_LINES;
                // The consumer chooses whether a damaged pair is fatal or can be skipped.
                if (!parser->frame_ready(parser->context, valid ? parser->frame : NULL, parser->field_callback_ns))
                    return false;
            }
            parser->lines[0] = parser->lines[1] = 0;
            parser->damaged = false;
            parser->collecting = true;
        }
        parser->seen_field = true;
        parser->field = field;
        parser->field_lines = 0;
        parser->blank_after_active = false;
    }

    // VBI codes and vertical blanking delimit fields without contributing picture bytes.
    bool active_sav = (marker & 0xb0) == 0x80;
    if (parser->collecting && active_sav) {
        if (parser->blank_after_active)
            parser->damaged = true;
        parser->line_size = 0;
        // Use first active SAV on each field. Blanking markers can arrive at a different phase.
        if (parser->lines[field] == 0)
            parser->field_callback_ns[field] = callback_ns;
        parser->active_line = true;
    } else if ((marker & 0x20) && parser->lines[field] != 0) {
        parser->blank_after_active = true;
    }
    return true;
}

static bool is_marker(uint8_t byte)
{
    static const uint8_t protection[] = {0, 0xd, 0xb, 6, 7, 0xa, 0xc, 1};
    unsigned flags = (byte >> 4) & 7;
    unsigned check = byte & 0x0f;
    // This bridge can omit BT.656 protection bits and clear bit 7 on raw VBI markers.
    if (!(byte & 0x80) && !(byte & 0x20))
        return false;
    return check == 0 || check == protection[flags];
}

bool cx231xx_video_feed(struct cx231xx_video *parser, const uint8_t *bytes, size_t size, uint64_t callback_ns)
{
    static const uint8_t preamble[] = {0xff, 0, 0};
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = bytes[i];
        unsigned prefix = parser->prefix;
        if (prefix == sizeof preamble) {
            if (is_marker(byte)) {
                parser->prefix = 0;
                if (!video_marker(parser, byte, callback_ns))
                    return false;
                continue;
            }
        } else if (byte == preamble[prefix]) {
            ++parser->prefix;
            continue;
        }
        // A false prefix belongs to the line. A fresh 0xff can still start the next marker.
        for (unsigned j = 0; j < prefix; ++j)
            line_byte(parser, preamble[j]);
        parser->prefix = 0;
        if (byte == 0xff)
            parser->prefix = 1;
        else
            line_byte(parser, byte);
    }
    return true;
}
