// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CX231XX_SESSION_H
#define CX231XX_SESSION_H

#include <signal.h>
#include "cx231xx-picture.h"
#include "cx231xx-record.h"

struct capture_monitor;

/** Startup picture values and whether to poll the terminal for live changes. */
struct picture_session {
    struct picture_settings initial;
    bool configured, terminal;
};

/** Capture options, borrowed for one blocking session. Zero seconds means run until stopped. */
struct capture_options {
    const char *firmware_path, *directory;
    unsigned seconds;
    enum capture_profile profile;
    bool claim, iso_test, initialize, stream, view_only, raw, matroska;
    bool svideo, preview, camera;
    struct picture_session picture;
    const struct capture_monitor *monitor;
    const volatile sig_atomic_t *signal_number;
};

/** Run the capture on the caller's worker thread. Returns an exit status after USB cleanup. */
int capture_run(const struct capture_options *options);

void print_picture_controls(void);

#endif
