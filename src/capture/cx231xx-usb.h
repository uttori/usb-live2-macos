// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef CX231XX_USB_H
#define CX231XX_USB_H

#include "cx231xx-init.h"

enum {
    HAUPPAUGE_VID = 0x2040,
    USB_LIVE2_PID = 0xc200,
    USB_LIVE2_REV = 0x4001,
    // At bInterval=1, 32 high-speed microframes cover 4 ms of audio/video.
    ISO_PACKETS = 32,
    USB_TIMEOUT_MS = 500,
    // 16 transfers keep 64 ms queued per endpoint to tolerate host scheduling jitter.
    STREAM_QUEUE = 16,
    // VID_STATUS bits 16 and 18 indicate horizontal and vertical lock.
    VIDEO_LOCK_BITS = 0x50000,
    // Four checks per second bound signal-loss detection without flooding control endpoint 0.
    VIDEO_LOCK_POLL_MS = 250,
};

void usb_error(const char *operation, int error);

/** Effective USB 2.0 high-speed periodic payload in bytes per service interval. */
unsigned high_speed_capacity(uint16_t max_packet);

/** Counters are updated only by the thread dispatching libusb events. */
struct stream_counts {
    uint64_t bytes;
    uint64_t packets;
    uint64_t empty;
    uint64_t packet_errors;
    uint64_t transfer_errors;
    uint64_t submit_errors;
    size_t pending;
    int first_error;
    const char *error_operation;
};

/** All state belongs to the session thread, including synchronous libusb completions. */
struct stream_run {
    bool stopping;
    bool measuring;
    bool sink_failed;
    bool device_lost;
    /** Borrows packet bytes until return. Timestamps are in ns from origin_ns. False stops the stream. */
    bool (*feed)(void *context, bool video, const uint8_t *bytes, size_t size, uint64_t timestamp);
    void *feed_context;
    const struct cx231xx_cancel *cancel;
    uint64_t origin_ns;
    struct stream_counts counts[2];
};

/** A submitted slot belongs to libusb until its callback, even after cancellation. */
struct stream_slot {
    struct libusb_transfer *transfer;
    struct stream_run *run;
    struct stream_counts *counts;
    bool pending;
};

/** Queue into zeroed slots. Always drain, including after a partial submission failure. */
bool queue_stream(libusb_device_handle *handle, struct stream_run *run,
                  struct stream_slot slots[2][STREAM_QUEUE]);

/** Dispatch completions on the session thread, waiting up to 100 ms. */
int stream_events(libusb_context *context);

/** Cancel and wait for every callback before freeing slots or restoring USB state. */
bool drain_stream(libusb_context *context, struct stream_run *run,
                  struct stream_slot slots[2][STREAM_QUEUE]);

/** Name of endpoint 0 (audio) or 1 (video) in the stream counters. */
const char *cx231xx_stream_name(size_t index);

/** Run on the USB session thread while the device claims or alternates are held. */
typedef bool (*cx231xx_stream_action)(libusb_context *context, libusb_device_handle *handle, void *data);

/** Borrow a fully claimed device for an optional action after firmware initialization. */
bool cx231xx_usb_access(libusb_context *context, libusb_device *device,
                       const struct libusb_device_descriptor *desc, bool iso_test,
                       const uint8_t *firmware, const struct cx231xx_cancel *cancel,
                       cx231xx_stream_action action, void *action_context);

/** Restore the original USB alternates even if the action or a SET_INTERFACE fails. */
bool cx231xx_with_stream_interfaces(libusb_context *context, libusb_device_handle *handle,
                                   const struct cx231xx_cancel *cancel, cx231xx_stream_action action,
                                   void *action_context, const char **reason);

/** Refresh an already read status until H/V lock or the settle deadline. False indicates I/O failure or stop. */
bool cx231xx_wait_for_video_lock(libusb_device_handle *handle, uint32_t *status,
                                const struct cx231xx_cancel *cancel);

#endif
