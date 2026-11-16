/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CX231XX_WRITER_H
#define CX231XX_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct capture_writer;

enum capture_profile {
    CAPTURE_FFV1, CAPTURE_PRORES, CAPTURE_H264, CAPTURE_H264_HW, CAPTURE_RAW, CAPTURE_PROFILE_COUNT
};

/** A fixed profile couples video and audio formats with container and recovery settings. */
const char *capture_profile_name(enum capture_profile profile);
const char *capture_media_name(enum capture_profile profile, bool complete);

/** Open an exclusive, growing partial file. Only the writer thread uses it after startup. */
int capture_writer_open(struct capture_writer **writer, const char *directory, enum capture_profile profile);

/** Write one woven video frame or a whole block of stereo PCM. PTS and duration are in ns. */
int capture_writer_write(struct capture_writer *writer, bool video, const uint8_t *bytes, size_t size,
              int64_t pts, int64_t duration);

/** Finalize if possible. On error truncate to the last completed cluster or fragment. */
int capture_writer_close(struct capture_writer *writer, int error, bool *finalized, uint64_t *bytes);

/** Publish a successfully finalized partial recording without overwriting a file. */
int capture_writer_publish(const char *directory, enum capture_profile profile);

#endif
