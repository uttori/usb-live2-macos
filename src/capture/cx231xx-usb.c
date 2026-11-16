// SPDX-License-Identifier: GPL-2.0-or-later
#define _DARWIN_C_SOURCE
#include "cx231xx-usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

/** Descriptor values for the analog interfaces on the measured high-speed revision. */
static const struct probe_interface {
    const char *name;
    uint8_t number;
    uint8_t alt;
    uint8_t endpoint;
    uint8_t type;
    uint16_t max_packet;
    uint8_t interval;
} interfaces[] = {
    // wMaxPacketSize encodes payload and transactions, not a flat byte count.
    {"board", 1, 0, 0x8f, LIBUSB_TRANSFER_TYPE_INTERRUPT, 0x0008, 7},
    {"audio", 2, 1, 0x83, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS, 0x001c, 1},
    // Alt 3 reserves three 964-byte transactions per 125 µs microframe (2,892 bytes).
    {"video", 3, 3, 0x84, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS, 0x13c4, 1},
};

void usb_error(const char *operation, int error)
{
    fprintf(stderr, "%s: %s (%d)\n", operation, libusb_error_name(error), error);
}

/** Effective USB 2.0 high-speed periodic payload in bytes per service interval. */
unsigned high_speed_capacity(uint16_t max_packet)
{
    // USB 2.0 wMaxPacketSize: bits 0–10 are bytes, bits 11–12 are additional transactions.
    return (max_packet & 0x07ff) * (1 + ((max_packet >> 11) & 3));
}

static bool matches_interface(const struct libusb_config_descriptor *config,
                              const struct probe_interface *expected)
{
    for (int i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int j = 0; j < iface->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];
            if (alt->bInterfaceNumber != expected->number || alt->bAlternateSetting != expected->alt)
                continue;
            if (alt->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC || alt->bNumEndpoints != 1)
                return false;
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[0];
            return ep->bEndpointAddress == expected->endpoint &&
                   (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == expected->type &&
                   ep->wMaxPacketSize == expected->max_packet && ep->bInterval == expected->interval;
        }
    }
    return false;
}

/** Require the measured interface layout before issuing any request to claim it. */
static bool check_active_layout(libusb_device *device, const struct libusb_device_descriptor *desc)
{
    if (desc->bcdDevice != USB_LIVE2_REV || desc->bcdUSB != 0x0200 ||
        desc->bNumConfigurations != 1 || libusb_get_device_speed(device) != LIBUSB_SPEED_HIGH) {
        fprintf(stderr, "Access probe requires USB-Live2 revision 0x%04x at USB 2.0 high speed.\n",
                USB_LIVE2_REV);
        return false;
    }
    struct libusb_config_descriptor *config;
    int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc < 0) {
        usb_error("libusb_get_active_config_descriptor", rc);
        return false;
    }

    bool ok = config->bConfigurationValue == 1 && config->bNumInterfaces == 6;
    if (!ok)
        fprintf(stderr, "Access probe requires active configuration 1 with six interfaces.\n");
    for (size_t i = 0; i < sizeof interfaces / sizeof interfaces[0]; ++i) {
        if (!matches_interface(config, &interfaces[i])) {
            fprintf(stderr, "Unexpected %s descriptor at interface %u alt %u endpoint 0x%02x.\n",
                    interfaces[i].name, interfaces[i].number, interfaces[i].alt, interfaces[i].endpoint);
            ok = false;
        }
    }
    libusb_free_config_descriptor(config);
    return ok;
}

/** Transfer and buffer stay owned by the probe until the completion callback runs. */
struct iso_read {
    struct libusb_transfer *transfer;
    bool completed;
    bool pending;
};

static void complete_read(struct libusb_transfer *transfer)
{
    struct iso_read *read = transfer->user_data;
    read->completed = true;
    read->pending = false;
}

/** Queue one short read on each A/V endpoint without enabling the device's streams. */
static bool probe_iso_reads(libusb_context *context, libusb_device_handle *handle,
                            const struct cx231xx_cancel *cancel)
{
    struct iso_read reads[2] = {0};
    const struct probe_interface *streams[] = {&interfaces[1], &interfaces[2]};
    bool ok = true;

    // This thread alone dispatches libusb events, so callbacks cannot race submissions.
    for (size_t i = 0; i < sizeof reads / sizeof reads[0] && !cx231xx_cancelled(cancel); ++i) {
        const struct probe_interface *stream = streams[i];
        unsigned capacity = high_speed_capacity(stream->max_packet);
        struct libusb_transfer *transfer = libusb_alloc_transfer(ISO_PACKETS);
        reads[i].transfer = transfer;
        unsigned char *buffer = malloc(capacity * ISO_PACKETS);
        if (!transfer || !buffer) {
            free(buffer);
            fprintf(stderr, "Could not allocate %s isochronous transfer.\n", stream->name);
            ok = false;
            break;
        }
        libusb_fill_iso_transfer(transfer, handle, stream->endpoint, buffer, (int)(capacity * ISO_PACKETS),
                                 ISO_PACKETS, complete_read, &reads[i], USB_TIMEOUT_MS);
        transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER;
        libusb_set_iso_packet_lengths(transfer, capacity);
        int rc = libusb_submit_transfer(transfer);
        printf("  Submit %s read: %s (%d)\n", stream->name, libusb_error_name(rc), rc);
        if (rc < 0) {
            ok = false;
            break;
        }
        reads[i].pending = true;
    }

    // On partial submission failure or interrupt, cancel pending reads and drain their callbacks.
    bool cancelled = false;
    while (reads[0].pending || reads[1].pending) {
        if ((!ok || cx231xx_cancelled(cancel)) && !cancelled) {
            for (size_t i = 0; i < sizeof reads / sizeof reads[0]; ++i) {
                if (!reads[i].pending)
                    continue;
                int rc = libusb_cancel_transfer(reads[i].transfer);
                if (rc < 0 && rc != LIBUSB_ERROR_NOT_FOUND)
                    usb_error("libusb_cancel_transfer", rc);
            }
            cancelled = true;
        }
        struct timeval wait = {.tv_usec = 100000};
        int rc = libusb_handle_events_timeout(context, &wait);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED && ok) {
            usb_error("libusb_handle_events_timeout", rc);
            ok = false;
        }
    }

    for (size_t i = 0; i < sizeof reads / sizeof reads[0]; ++i) {
        struct libusb_transfer *transfer = reads[i].transfer;
        // Packet statuses are meaningful only after a successful transfer completion.
        if (reads[i].completed) {
            unsigned bytes = 0;
            unsigned failed = 0;
            unsigned empty = 0;
            if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
                for (int p = 0; p < transfer->num_iso_packets; ++p) {
                    const struct libusb_iso_packet_descriptor *packet = &transfer->iso_packet_desc[p];
                    if (packet->status != LIBUSB_TRANSFER_COMPLETED) {
                        printf("  %s packet %d: %s (%d)\n", streams[i]->name, p,
                               libusb_error_name((int)packet->status), packet->status);
                        ++failed;
                    } else {
                        bytes += packet->actual_length;
                        empty += packet->actual_length == 0;
                    }
                }
            }
            printf("  %s completion: %s (%d)", streams[i]->name,
                   libusb_error_name((int)transfer->status), transfer->status);
            if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
                printf(", %u bytes, %u empty packets, %u packet errors", bytes, empty, failed);
            putchar('\n');
            if (transfer->status != LIBUSB_TRANSFER_COMPLETED || failed > 0)
                ok = false;
        }
        libusb_free_transfer(transfer);
    }
    return ok && !cx231xx_cancelled(cancel);
}

static void stream_error(struct stream_counts *counts, const char *operation, int code)
{
    if (!counts->error_operation) {
        counts->error_operation = operation;
        counts->first_error = code;
    }
}

static bool submit_stream_slot(struct stream_slot *slot)
{
    int rc = libusb_submit_transfer(slot->transfer);
    if (rc < 0) {
        ++slot->counts->submit_errors;
        stream_error(slot->counts, "submit", rc);
        slot->run->device_lost = rc == LIBUSB_ERROR_NO_DEVICE;
        slot->run->stopping = true;
        return false;
    }
    slot->pending = true;
    ++slot->counts->pending;
    return true;
}

static void stream_complete(struct libusb_transfer *transfer)
{
    struct stream_slot *slot = transfer->user_data;
    struct stream_counts *counts = slot->counts;
    slot->pending = false;
    --counts->pending;
    // Cancellation can abort the entire endpoint on macOS. Those completions are expected at stop.
    if (slot->run->stopping)
        return;
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        ++counts->transfer_errors;
        stream_error(counts, "transfer", (int)transfer->status);
        slot->run->device_lost = transfer->status == LIBUSB_TRANSFER_NO_DEVICE;
        slot->run->stopping = true;
        return;
    }
    // Take one timestamp before parsing. Libusb's public packet descriptors have no bus timestamp.
    uint64_t callback_ns = 0;
    if (slot->run->feed && slot->run->measuring)
        callback_ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - slot->run->origin_ns;
    for (int i = 0; i < transfer->num_iso_packets; ++i) {
        const struct libusb_iso_packet_descriptor *packet = &transfer->iso_packet_desc[i];
        if (packet->status != LIBUSB_TRANSFER_COMPLETED || packet->actual_length > packet->length) {
            ++counts->packet_errors;
            int status = (int)packet->status;
            if (status == LIBUSB_TRANSFER_COMPLETED)
                status = LIBUSB_TRANSFER_OVERFLOW;
            stream_error(counts, "packet", status);
            // A lost packet can break field boundaries or PCM alignment. Keep the completed prefix only.
            if (slot->run->feed) {
                slot->run->device_lost = status == LIBUSB_TRANSFER_NO_DEVICE;
                slot->run->stopping = true;
                return;
            }
        } else if (slot->run->measuring) {
            counts->bytes += packet->actual_length;
            ++counts->packets;
            counts->empty += packet->actual_length == 0;
            if (slot->run->feed && packet->actual_length &&
                !slot->run->feed(slot->run->feed_context, transfer->endpoint == interfaces[2].endpoint,
                                 libusb_get_iso_packet_buffer_simple(transfer, (unsigned)i),
                                 packet->actual_length, callback_ns)) {
                slot->run->sink_failed = true;
                slot->run->stopping = true;
                return;
            }
        }
    }
    // Keep the queue full without allocating or logging from a callback.
    if (!cx231xx_cancelled(slot->run->cancel))
        submit_stream_slot(slot);
}

bool queue_stream(libusb_device_handle *handle, struct stream_run *run,
                  struct stream_slot slots[2][STREAM_QUEUE])
{
    for (size_t i = 0; i < 2; ++i) {
        const struct probe_interface *stream = &interfaces[i + 1];
        unsigned capacity = high_speed_capacity(stream->max_packet);
        for (size_t j = 0; j < STREAM_QUEUE; ++j) {
            struct stream_slot *slot = &slots[i][j];
            slot->run = run;
            slot->counts = &run->counts[i];
            slot->transfer = libusb_alloc_transfer(ISO_PACKETS);
            unsigned char *buffer = malloc(capacity * ISO_PACKETS);
            if (!slot->transfer || !buffer) {
                free(buffer);
                fprintf(stderr, "Could not allocate %s stream transfer.\n", stream->name);
                return false;
            }
            libusb_fill_iso_transfer(slot->transfer, handle, stream->endpoint, buffer,
                                     (int)(capacity * ISO_PACKETS), ISO_PACKETS, stream_complete,
                                     slot, USB_TIMEOUT_MS);
            slot->transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER;
            libusb_set_iso_packet_lengths(slot->transfer, capacity);
            if (cx231xx_cancelled(run->cancel) || !submit_stream_slot(slot))
                return false;
        }
    }
    return true;
}

int stream_events(libusb_context *context)
{
    struct timeval wait = {.tv_usec = 100000};
    return libusb_handle_events_timeout(context, &wait);
}

bool drain_stream(libusb_context *context, struct stream_run *run,
                  struct stream_slot slots[2][STREAM_QUEUE])
{
    run->stopping = true;
    bool ok = true;
    for (size_t i = 0; i < 2; ++i) {
        for (size_t j = 0; j < STREAM_QUEUE; ++j) {
            if (!slots[i][j].pending)
                continue;
            int rc = libusb_cancel_transfer(slots[i][j].transfer);
            if (rc < 0 && rc != LIBUSB_ERROR_NOT_FOUND && rc != LIBUSB_ERROR_NO_DEVICE) {
                usb_error("libusb_cancel_transfer", rc);
                ok = false;
            }
        }
    }
    // Do not restore alternates or free buffers while the host can still complete a read.
    while (run->counts[0].pending || run->counts[1].pending) {
        int rc = stream_events(context);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED && ok) {
            usb_error("libusb_handle_events_timeout during drain", rc);
            ok = false;
        }
    }
    for (size_t i = 0; i < 2; ++i)
        for (size_t j = 0; j < STREAM_QUEUE; ++j)
            libusb_free_transfer(slots[i][j].transfer);
    return ok;
}

/** Exercise USB alternate settings and restore each setting after draining the reads. */
bool cx231xx_with_stream_interfaces(libusb_context *context, libusb_device_handle *handle,
                                   const struct cx231xx_cancel *cancel, cx231xx_stream_action action,
                                   void *action_context, const char **reason)
{
    const struct probe_interface *streams[] = {&interfaces[1], &interfaces[2]};
    uint8_t original[2];
    for (size_t i = 0; i < sizeof streams / sizeof streams[0]; ++i) {
        int rc = libusb_control_transfer(handle, (uint8_t)(LIBUSB_ENDPOINT_IN | (unsigned)LIBUSB_RECIPIENT_INTERFACE),
                                         LIBUSB_REQUEST_GET_INTERFACE, 0, streams[i]->number,
                                         &original[i], 1, USB_TIMEOUT_MS);
        if (rc != 1) {
            fprintf(stderr, "GET_INTERFACE %u: expected one byte, got %d\n", streams[i]->number, rc);
            if (rc < 0)
                usb_error("libusb_control_transfer", rc);
            return false;
        }
    }

    bool ok = true;
    size_t changed = 0;
    for (size_t i = 0; i < sizeof streams / sizeof streams[0] && !cx231xx_cancelled(cancel); ++i) {
        // A backend error can follow a successful SET_INTERFACE, so restore attempted settings too.
        ++changed;
        int rc = libusb_set_interface_alt_setting(handle, streams[i]->number, streams[i]->alt);
        printf("  Set interface %u alt %u: %s (%d)\n", streams[i]->number, streams[i]->alt,
               libusb_error_name(rc), rc);
        if (rc < 0) {
            ok = false;
            break;
        }
    }
    if (ok && !cx231xx_cancelled(cancel)) {
        if (action)
            ok = action(context, handle, action_context);
        else
            ok = probe_iso_reads(context, handle, cancel);
    }
    while (changed > 0) {
        --changed;
        uint8_t number = streams[changed]->number;
        int rc = libusb_set_interface_alt_setting(handle, number, original[changed]);
        printf("  Restore interface %u alt %u: %s (%d)\n", number, original[changed],
               libusb_error_name(rc), rc);
        if (rc < 0) {
            ok = false;
            if (reason && rc == LIBUSB_ERROR_NO_DEVICE)
                *reason = "device_unplugged";
        }
    }
    return ok && !cx231xx_cancelled(cancel);
}

/** Hold the board, audio and video interfaces concurrently, then release any acquired claims. */
bool cx231xx_usb_access(libusb_context *context, libusb_device *device,
                       const struct libusb_device_descriptor *desc, bool iso_test,
                       const uint8_t *firmware, const struct cx231xx_cancel *cancel,
                       cx231xx_stream_action action, void *action_context)
{
    if (!check_active_layout(device, desc))
        return false;

    libusb_device_handle *handle;
    int rc = libusb_open(device, &handle);
    if (rc < 0) {
        usb_error("libusb_open", rc);
        return false;
    }
    puts("  libusb_open: success");

    const size_t interface_count = sizeof interfaces / sizeof interfaces[0];
    for (size_t i = 0; i < interface_count; ++i) {
        uint8_t number = interfaces[i].number;
        rc = libusb_kernel_driver_active(handle, number);
        printf("  Interface %u (%s) kernel driver: ", number, interfaces[i].name);
        if (rc < 0)
            printf("%s (%d)\n", libusb_error_name(rc), rc);
        else
            puts(rc == 1 ? "active" : "none reported");
    }

    size_t claimed = 0;
    for (; claimed < interface_count; ++claimed) {
        uint8_t number = interfaces[claimed].number;
        rc = libusb_claim_interface(handle, number);
        printf("  Claim interface %u: %s (%d)\n", number, libusb_error_name(rc), rc);
        if (rc < 0)
            break;
    }
    bool ok = claimed == interface_count;
    if (ok) {
        puts("  Board, audio and video interfaces held concurrently.");
        if (iso_test)
            ok = cx231xx_with_stream_interfaces(context, handle, cancel, NULL, NULL, NULL);
        if (firmware)
            ok = cx231xx_init_board(handle, firmware, cancel);
        if (ok && action && !cx231xx_cancelled(cancel))
            ok = action(context, handle, action_context);
    }

    // Release only the successful claims if a later claim failed.
    while (claimed > 0) {
        uint8_t number = interfaces[--claimed].number;
        rc = libusb_release_interface(handle, number);
        printf("  Release interface %u: %s (%d)\n", number, libusb_error_name(rc), rc);
        if (rc < 0)
            ok = false;
    }
    libusb_close(handle);
    return ok;
}

const char *cx231xx_stream_name(size_t index)
{
    return interfaces[index + 1].name;
}

bool cx231xx_wait_for_video_lock(libusb_device_handle *handle, uint32_t *status,
                                const struct cx231xx_cancel *cancel)
{
    // Routing can temporarily clear decoder lock. Give it one second to settle.
    uint64_t deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + 1000000000;
    while ((*status & VIDEO_LOCK_BITS) != VIDEO_LOCK_BITS && !cx231xx_cancelled(cancel) &&
           clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) < deadline) {
        const struct timespec settle = {.tv_nsec = 100000000};
        nanosleep(&settle, NULL);
        if (!cx231xx_video_status(handle, status))
            return false;
    }
    return !cx231xx_cancelled(cancel);
}
