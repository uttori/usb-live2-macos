/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CX231XX_MONITOR_H
#define CX231XX_MONITOR_H

#include <stddef.h>
#include "cx231xx-picture.h"

struct recorder;
struct capture_stats;

/** Optional in-process preview sink. Sample pointers expire when each callback returns. */
struct capture_monitor {
    void *context;
    void (*video)(void *context, const uint8_t *frame);
    void (*audio)(void *context, const uint8_t *pcm, size_t size);
    /** Copy the latest desired settings. Return false to stop and release USB. */
    bool (*settings)(void *context, struct picture_settings *settings);
    /** Transfer a prepared recorder to the capture thread, or request that the active one stop. */
    struct recorder *(*recording)(void *context, bool *stop);
    /** Take a detached recorder. Copy stats before returning. The stop reason is a static string. */
    void (*finish_recording)(void *context, struct recorder *recorder,
                             const struct capture_stats *stats, const char *reason, bool complete);
};

#endif
