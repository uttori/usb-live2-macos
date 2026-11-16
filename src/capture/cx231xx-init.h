/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CX231XX_INIT_H
#define CX231XX_INIT_H

#include <libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include "cx231xx-picture.h"

enum { CX231XX_FIRMWARE_SIZE = 16382 };

/** Optional stop check, borrowed for the current USB operation. */
struct cx231xx_cancel {
    bool (*requested)(void *context);
    void *context;
};

static inline bool cx231xx_cancelled(const struct cx231xx_cancel *cancel)
{
    return cancel && cancel->requested(cancel->context);
}

/** Read an image of exactly CX231XX_FIRMWARE_SIZE bytes. NULL selects the shipped Linux image. */
bool cx231xx_read_firmware(const char *path, uint8_t image[static CX231XX_FIRMWARE_SIZE]);

/**
 * Initialize the USB-Live2 bridge and decoder with interfaces 1–3 already claimed.
 * Success leaves the decoder powered and running with USB A/V endpoints disabled.
 */
bool cx231xx_init_board(libusb_device_handle *handle,
                       const uint8_t image[static CX231XX_FIRMWARE_SIZE],
                       const struct cx231xx_cancel *cancel);

/** Route composite or S-Video at NTSC-M, with stereo line input for either connector. */
bool cx231xx_route_input(libusb_device_handle *handle, bool svideo);

/** Apply and read back picture controls without changing the input route or stream mode. */
bool cx231xx_set_picture(libusb_device_handle *handle, const struct picture_settings *settings);

/** Snapshot bridge state before any stream writes. */
struct cx231xx_stream_state {
    uint32_t mode;
    uint32_t endpoints;
};

bool cx231xx_read_stream_state(libusb_device_handle *handle, struct cx231xx_stream_state *state);

/** Enable A/V on the selected isochronous alternates, or restore the saved state. */
bool cx231xx_set_streams(libusb_device_handle *handle, const struct cx231xx_stream_state *saved,
                         bool enable);

/** Read the decoder's raw video status word, including horizontal and vertical lock. */
bool cx231xx_video_status(libusb_device_handle *handle, uint32_t *status);

#endif
