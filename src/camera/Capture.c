// SPDX-License-Identifier: GPL-2.0-or-later
#define _DARWIN_C_SOURCE
#include "Capture.h"
#include "Fields.h"
#include "cx231xx-usb.h"

#include <stdlib.h>
#include <time.h>

/** One attempt owns the parser and image pool until the USB transfer callbacks have drained. */
struct camera_capture_run {
    struct cx231xx_cancel cancel;
    void (*deliver)(void *, CMSampleBufferRef);
    struct camera_fields *fields;
    struct cx231xx_video parser;
    const char *reason;
};

static bool deliver_frame(void *context, const uint8_t *frame, const uint64_t field_ns[2])
{
    (void)field_ns;
    struct camera_capture_run *capture = context;
    // A viewer can resume at the next intact field pair after damage to the analog signal.
    if (!frame || cx231xx_cancelled(&capture->cancel))
        return true;
    CMTime times[2];
    camera_field_times(capture->fields, &times[0], &times[1]);
    for (unsigned parity = 0; parity < 2; ++parity) {
        CMSampleBufferRef sample = camera_make_field(capture->fields, frame, parity, times[parity]);
        if (sample) {
            capture->deliver(capture->cancel.context, sample);
            CFRelease(sample);
        }
    }
    return true;
}

static bool feed_camera(void *context, bool video, const uint8_t *bytes, size_t size, uint64_t timestamp)
{
    struct camera_capture_run *capture = context;
    return !video || cx231xx_video_feed(&capture->parser, bytes, size, timestamp);
}

static bool stream_camera(libusb_context *context, libusb_device_handle *handle, void *data)
{
    struct camera_capture_run *capture = data;
    struct cx231xx_stream_state saved;
    if (!cx231xx_read_stream_state(handle, &saved))
        return false;

    struct stream_run run = {.feed = feed_camera, .feed_context = capture, .cancel = &capture->cancel};
    struct stream_slot slots[2][STREAM_QUEUE] = {0};
    bool attempted_enable = queue_stream(handle, &run, slots) && !cx231xx_cancelled(run.cancel);
    bool ok = attempted_enable && cx231xx_set_streams(handle, &saved, true);
    run.origin_ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    // Start feeding the parser only after the hardware has enabled both endpoints.
    run.measuring = ok;
    uint64_t next_lock_check = run.origin_ns + VIDEO_LOCK_POLL_MS * 1000000ULL;
    while (ok && !run.stopping && !cx231xx_cancelled(run.cancel)) {
        int rc = stream_events(context);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
            usb_error("libusb_handle_events_timeout", rc);
            break;
        }
        // The bridge can continue sending data after the analog input drops out.
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        if (now >= next_lock_check && !cx231xx_cancelled(run.cancel)) {
            uint32_t status;
            if (!cx231xx_video_status(handle, &status))
                break;
            if ((status & VIDEO_LOCK_BITS) != VIDEO_LOCK_BITS) {
                capture->reason = "NTSC signal lost";
                break;
            }
            next_lock_check = now + VIDEO_LOCK_POLL_MS * 1000000ULL;
        }
    }
    // Transfers still own the buffers after cancellation. Drain before altering bridge state.
    bool drained = drain_stream(context, &run, slots);
    bool restored = !attempted_enable || cx231xx_set_streams(handle, &saved, false);
    return drained && restored && cx231xx_cancelled(run.cancel);
}

static bool route_camera(libusb_context *context, libusb_device_handle *handle, void *data)
{
    struct camera_capture_run *capture = data;
    const bool inputs[] = {true, false};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
        uint32_t status;
        if (cx231xx_cancelled(&capture->cancel) || !cx231xx_route_input(handle, inputs[i]) ||
            !cx231xx_video_status(handle, &status) ||
            !cx231xx_wait_for_video_lock(handle, &status, &capture->cancel))
            return false;
        // Use composite only when S-Video has no lock, so a connected S-Video source takes priority.
        if ((status & VIDEO_LOCK_BITS) == VIDEO_LOCK_BITS) {
            return cx231xx_with_stream_interfaces(context, handle, &capture->cancel,
                                                 stream_camera, capture, NULL);
        }
    }
    capture->reason = "no NTSC signal on S-Video or composite";
    return false;
}

const char *camera_capture(const char *firmware, bool (*stopped)(void *),
                           void (*deliver)(void *, CMSampleBufferRef), void *context)
{
    uint8_t image[CX231XX_FIRMWARE_SIZE];
    if (!cx231xx_read_firmware(firmware, image))
        return "could not read camera firmware";
    libusb_context *usb;
    int rc = libusb_init(&usb);
    if (rc < 0) {
        usb_error("libusb_init", rc);
        return "could not initialize libusb";
    }
    libusb_device **devices;
    ssize_t count = libusb_get_device_list(usb, &devices);
    if (count < 0) {
        usb_error("libusb_get_device_list", (int)count);
        libusb_exit(usb);
        return "could not enumerate USB devices";
    }

    const char *reason = "no USB-Live2 device";
    for (ssize_t i = 0; i < count && !stopped(context); ++i) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devices[i], &desc) < 0 ||
            desc.idVendor != HAUPPAUGE_VID || desc.idProduct != USB_LIVE2_PID)
            continue;
        struct camera_capture_run *capture = calloc(1, sizeof *capture);
        if (!capture) {
            reason = "could not allocate camera parser";
            break;
        }
        capture->cancel = (struct cx231xx_cancel){.requested = stopped, .context = context};
        capture->deliver = deliver;
        capture->parser.frame_ready = deliver_frame;
        capture->parser.context = capture;
        capture->reason = "USB-Live2 capture failed";
        capture->fields = camera_fields_create(true);
        if (capture->fields) {
            cx231xx_usb_access(usb, devices[i], &desc, false, image, &capture->cancel, route_camera, capture);
            reason = capture->reason;
            camera_fields_destroy(capture->fields);
        } else {
            reason = "could not allocate camera frames";
        }
        free(capture);
        break;
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(usb);
    return stopped(context) ? NULL : reason;
}
