// SPDX-License-Identifier: GPL-2.0-or-later
#define _DARWIN_C_SOURCE
#include "cx231xx-session.h"
#include "cx231xx-usb.h"
#include "cx231xx-monitor.h"
#include "cx231xx-raw.h"
#include "cx231xx-view.h"

#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Coalesce slider/terminal changes at 20 Hz instead of writing registers for every UI event.
enum { PICTURE_POLL_MS = 50 };

/** Report cached descriptors without opening or changing the device. */
static void describe_layout(const struct libusb_config_descriptor *config, int speed)
{
    static const char *const types[] = {"control", "isochronous", "bulk", "interrupt"};

    printf("  Configuration %u, %u interfaces\n", config->bConfigurationValue,
           config->bNumInterfaces);
    for (int i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int j = 0; j < iface->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];
            printf("    Interface %u alt %u class %02x/%02x/%02x, %u endpoints\n",
                   alt->bInterfaceNumber, alt->bAlternateSetting, alt->bInterfaceClass,
                   alt->bInterfaceSubClass, alt->bInterfaceProtocol, alt->bNumEndpoints);
            for (int k = 0; k < alt->bNumEndpoints; ++k) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                unsigned type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                printf("      0x%02x %-3s %-11s wMaxPacketSize=0x%04x bInterval=%u",
                       ep->bEndpointAddress, ep->bEndpointAddress & LIBUSB_ENDPOINT_IN ? "IN" : "OUT",
                       types[type], ep->wMaxPacketSize, ep->bInterval);
                // High-speed periodic endpoints can carry several transactions per interval.
                if (speed == LIBUSB_SPEED_HIGH &&
                    (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS || type == LIBUSB_TRANSFER_TYPE_INTERRUPT)) {
                    printf(" (%u bytes/interval)", high_speed_capacity(ep->wMaxPacketSize));
                }
                putchar('\n');
            }
        }
    }
}

static void describe_device(libusb_device *device, const struct libusb_device_descriptor *desc)
{
    uint8_t ports[8];
    int count = libusb_get_port_numbers(device, ports, sizeof ports);
    int speed = libusb_get_device_speed(device);
    printf("Hauppauge USB-Live2 %04x:%04x, bcdDevice=0x%04x, bcdUSB=0x%04x\n",
           desc->idVendor, desc->idProduct, desc->bcdDevice, desc->bcdUSB);
    printf("  Bus %u address %u", libusb_get_bus_number(device),
           libusb_get_device_address(device));
    if (count > 0) {
        printf(", port path %u", ports[0]);
        for (int i = 1; i < count; ++i)
            printf(".%u", ports[i]);
    }
    printf(", USB speed %d", speed);
    if (speed == LIBUSB_SPEED_HIGH)
        printf(" (high speed, 480 Mb/s)");
    putchar('\n');
}

static double monotonic_seconds(void)
{
    return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9;
}

void print_picture_controls(void)
{
    for (int i = 0; i < PICTURE_CONTROL_COUNT; ++i) {
        const struct picture_control *control = &picture_controls[i];
        printf("%-16s %4d to %-4d  default %d\n", control->name, control->min,
               control->max, control->initial);
    }
}

/** Poll the terminal without stalling USB. Invalid commands leave the settings intact. */
static void read_picture_command(struct picture_settings *settings)
{
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&input, 1, 0) <= 0 || !(input.revents & POLLIN))
        return;

    // The terminal delivers complete lines, so one read can reject a pasted value as a unit.
    char command[MAX_CANON];
    ssize_t count = read(STDIN_FILENO, command, sizeof command);
    if (count <= 0)
        return;
    if (command[count - 1] == '\n') {
        command[count - 1] = '\0';
        if (count == 2 && command[0] >= '0' && command[0] <= '4') {
            settings->value[PICTURE_PEAKING] = command[0] - '0';
            return;
        }
        if (strcmp(command, "reset") == 0) {
            *settings = picture_defaults();
            return;
        }
        if (strcmp(command, "help") == 0) {
            print_picture_controls();
            return;
        }
        if (picture_parse_setting(settings, command))
            return;
    }
    fputs("Enter name=value, 0–4 for peaking, reset, or help.\n", stderr);
}

/** Sample cumulative transport counters at a clip boundary on the USB thread. */
static void stream_stats(struct capture_stats *stats, const struct stream_run *run)
{
    for (size_t i = 0; i < 2; ++i) {
        const struct stream_counts *counts = &run->counts[i];
        stats->bytes[i] = counts->bytes;
        stats->packet_errors[i] = counts->packet_errors;
        stats->transfer_errors[i] = counts->transfer_errors;
        stats->submit_errors[i] = counts->submit_errors;
        stats->usb_status[i] = counts->first_error;
    }
}

static void finish_monitor_recording(const struct capture_monitor *monitor, struct recorder *recorder,
                                     struct raw_dump *dump, const struct stream_run *run, const struct capture_stats *begin,
                                     uint32_t video_lock, const char *reason, bool complete)
{
    struct capture_stats clip = {
        .origin_ns = begin->origin_ns,
        .elapsed = (double)(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - begin->origin_ns) / 1e9,
        .initial_video_lock = begin->initial_video_lock,
        .last_video_lock = video_lock,
    };
    stream_stats(&clip, run);
    for (size_t i = 0; i < 2; ++i) {
        clip.bytes[i] -= begin->bytes[i];
        clip.packet_errors[i] -= begin->packet_errors[i];
        clip.transfer_errors[i] -= begin->transfer_errors[i];
        clip.submit_errors[i] -= begin->submit_errors[i];
    }
    raw_dump_stop_recording(dump, &clip);
    monitor->finish_recording(monitor->context, recorder, &clip, reason, complete);
}

static bool feed_dump(void *context, bool video, const uint8_t *bytes, size_t size, uint64_t timestamp)
{
    return raw_dump_feed(context, video, bytes, size, timestamp);
}

/** Borrowed state for one capture on the selected device. */
struct capture_session {
    const struct capture_options *options;
    const struct cx231xx_cancel *cancel;
    struct raw_dump *dump;
    struct capture_stats *stats;
    const char **reason;
};

static bool probe_stream(libusb_context *context, libusb_device_handle *handle, void *data)
{
    const struct capture_session *session = data;
    const struct cx231xx_cancel *cancel = session->cancel;
    const struct picture_session *picture = &session->options->picture;
    const struct capture_monitor *monitor = session->options->monitor;
    struct raw_dump *dump = session->dump;
    struct capture_stats *stats = session->stats;
    const char **reason = session->reason;
    unsigned seconds = session->options->seconds;
    struct cx231xx_stream_state saved;
    uint32_t status;
    if (!cx231xx_video_status(handle, &status) || !cx231xx_read_stream_state(handle, &saved))
        return false;
    if (stats) {
        if (!cx231xx_wait_for_video_lock(handle, &status, cancel))
            return false;
        stats->initial_video_lock = stats->last_video_lock = status & VIDEO_LOCK_BITS;
        if (stats->initial_video_lock != VIDEO_LOCK_BITS) {
            fprintf(stderr, "Input did not acquire H/V lock (0x%05x).\n", stats->initial_video_lock);
            *reason = "no_video_lock";
            return false;
        }
    }
    printf("  Decoder video status before stream: 0x%08x (H/V lock bits 0x%05x)\n",
           status, status & VIDEO_LOCK_BITS);

    struct picture_settings applied = picture->initial;
    if (monitor && !monitor->settings(monitor->context, &applied))
        return true;
    bool controls_active = picture->configured || picture->terminal || monitor;
    if (controls_active && !cx231xx_set_picture(handle, &applied)) {
        *reason = "picture_control_error";
        return false;
    }
    struct stream_run run = {.feed = dump ? feed_dump : NULL, .feed_context = dump, .cancel = cancel};
    struct stream_slot slots[2][STREAM_QUEUE] = {0};
    bool attempted_enable = queue_stream(handle, &run, slots) && !cx231xx_cancelled(cancel);
    bool ok = attempted_enable && cx231xx_set_streams(handle, &saved, true);
    run.origin_ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (dump)
        raw_dump_set_origin(dump, run.origin_ns);
    double start = (double)run.origin_ns / 1e9;
    double elapsed = 0;
    double next_lock_check = (double)VIDEO_LOCK_POLL_MS / 1000.0;
    double next_picture_check = 0;
    const char *stream_failure = NULL;
    bool view_closed = false;
    struct recorder *monitor_recorder = NULL;
    struct capture_stats clip_begin = {0};
    clock_t cpu_start = clock();
    uint64_t previous[2] = {0};
    uint64_t window = 1;
    uint64_t empty_windows[2] = {0};
    // Synchronous stream-enable requests can dispatch callbacks for already queued reads.
    run.measuring = ok;
    if (ok && seconds)
        printf("  Streaming for %u seconds with %u transfers × %u packets queued per endpoint.\n",
               seconds, STREAM_QUEUE, ISO_PACKETS);
    if (ok && !seconds)
        printf("  Streaming until stopped with %u transfers × %u packets queued per endpoint.\n",
               STREAM_QUEUE, ISO_PACKETS);
    if (ok && picture->terminal)
        puts("  Picture controls: enter name=value, 0–4 for peaking, reset, or help.");

    while (ok && !run.stopping && !cx231xx_cancelled(cancel) && (!seconds || elapsed < seconds) &&
           !raw_dump_live_failed(dump)) {
        int rc = stream_events(context);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
            usb_error("libusb_handle_events_timeout", rc);
            run.device_lost |= rc == LIBUSB_ERROR_NO_DEVICE;
            ok = false;
        }
        elapsed = monotonic_seconds() - start;
        // A decoder can keep producing USB payloads after the analog source loses lock.
        if (stats && ok && !run.stopping && !cx231xx_cancelled(cancel) &&
            (elapsed >= next_lock_check || (seconds && elapsed >= seconds))) {
            if (!cx231xx_video_status(handle, &status)) {
                stream_failure = "video_status_error";
            } else {
                stats->last_video_lock = status & VIDEO_LOCK_BITS;
                if (stats->last_video_lock != VIDEO_LOCK_BITS) {
                    fprintf(stderr, "Input lost H/V lock (0x%05x) at %.3fs.\n",
                            stats->last_video_lock, elapsed);
                    stream_failure = "video_lock_lost";
                }
            }
            if (stream_failure)
                break;
            if (raw_dump_view_closed(dump)) {
                view_closed = true;
                break;
            }
            next_lock_check = elapsed + (double)VIDEO_LOCK_POLL_MS / 1000.0;
        }
        if ((monitor || picture->terminal) && ok && !run.stopping && !cx231xx_cancelled(cancel) &&
            elapsed >= next_picture_check) {
            next_picture_check = elapsed + (double)PICTURE_POLL_MS / 1000.0;
            struct picture_settings desired = applied;
            if (monitor && !monitor->settings(monitor->context, &desired)) {
                view_closed = true;
                break;
            }
            if (picture->terminal)
                read_picture_command(&desired);
            // Compare the whole snapshot so rapid slider movements coalesce into one update.
            if (memcmp(desired.value, applied.value, sizeof applied.value)) {
                if (!cx231xx_set_picture(handle, &desired)) {
                    stream_failure = "picture_control_error";
                    break;
                }
                applied = desired;
            }
            if (monitor && monitor->recording) {
                bool stop = false;
                struct recorder *prepared = monitor->recording(monitor->context, &stop);
                // Start only after the current picture settings are verified on the decoder.
                if (prepared) {
                    monitor_recorder = prepared;
                    clip_begin.origin_ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
                    clip_begin.initial_video_lock = stats->last_video_lock;
                    stream_stats(&clip_begin, &run);
                    raw_dump_start_recording(dump, prepared, clip_begin.origin_ns - run.origin_ns);
                }
                if (monitor_recorder && (stop || recorder_error(monitor_recorder) < 0)) {
                    finish_monitor_recording(monitor, monitor_recorder, dump, &run, &clip_begin,
                                             stats->last_video_lock, "user_stopped", true);
                    monitor_recorder = NULL;
                }
            }
        }
        if (elapsed < (double)window)
            continue;
        // Count payload-free intervals independently. Missed sampling boundaries also fail the run.
        uint64_t missed = (uint64_t)elapsed - window;
        for (size_t i = 0; i < 2; ++i) {
            empty_windows[i] += missed + (run.counts[i].bytes == previous[i]);
            previous[i] = run.counts[i].bytes;
        }
        if (window % 30 == 0 || (seconds && elapsed >= seconds))
            printf("  %.1fs: audio %.0f B/s, video %.0f B/s, packet errors %" PRIu64 "\n",
                   elapsed, (double)previous[0] / elapsed, (double)previous[1] / elapsed,
                   run.counts[0].packet_errors + run.counts[1].packet_errors);
        window = (uint64_t)elapsed + 1;
    }
    double cpu_seconds = (double)(clock() - cpu_start) / CLOCKS_PER_SEC;
    bool drained = drain_stream(context, &run, slots);
    bool restored = !attempted_enable || cx231xx_set_streams(handle, &saved, false);
    if (restored && attempted_enable)
        puts("  Restored USB stream registers.");
    if (controls_active && !run.device_lost) {
        struct picture_settings defaults = picture_defaults();
        if (!cx231xx_set_picture(handle, &defaults))
            stream_failure = "picture_reset_failed";
    }
    if (restored && cx231xx_video_status(handle, &status))
        printf("  Decoder video status after stream: 0x%08x (H/V lock bits 0x%05x)\n",
               status, status & VIDEO_LOCK_BITS);
    for (size_t i = 0; i < 2; ++i) {
        const struct stream_counts *counts = &run.counts[i];
        printf("  %s: %" PRIu64 " bytes (%.0f B/s), %" PRIu64 " packets, %" PRIu64
               " empty, %" PRIu64 " packet errors, %" PRIu64 " transfer errors, %" PRIu64
               " submission errors, %" PRIu64 " windows without confirmed payload\n",
               cx231xx_stream_name(i), counts->bytes, elapsed > 0 ? (double)counts->bytes / elapsed : 0,
               counts->packets, counts->empty, counts->packet_errors, counts->transfer_errors,
               counts->submit_errors, empty_windows[i]);
        if (counts->error_operation)
            fprintf(stderr, "%s first %s error: %s (%d)\n", cx231xx_stream_name(i),
                    counts->error_operation, libusb_error_name(counts->first_error), counts->first_error);
        if (!counts->bytes || empty_windows[i] || counts->error_operation)
            ok = false;
    }
    printf("  Elapsed %.3fs, process CPU %.3fs\n", elapsed, cpu_seconds);
    if (run.sink_failed && !stats)
        fputs("Raw dump buffer filled before the requested stop.\n", stderr);
    if (stats) {
        stream_stats(stats, &run);
        stats->elapsed = elapsed;
        if (stream_failure)
            *reason = stream_failure;
        else if (run.sink_failed)
            *reason = "parser_discontinuity";
        // Ctrl-C can arrive before both endpoints deliver their first payloads.
        else if (cx231xx_cancelled(cancel) && drained && restored &&
                 !run.counts[0].error_operation && !run.counts[1].error_operation)
            *reason = "interrupted";
        else if (ok && drained && restored)
            *reason = "duration_reached";
        else
            *reason = "usb_error";
        if (run.device_lost) {
            *reason = "device_unplugged";
        } else if (!ok || !drained || !restored || stream_failure) {
            // Darwin can report a generic transfer error on unplug. Check the device list.
            libusb_device **devices;
            ssize_t count = libusb_get_device_list(context, &devices);
            if (count >= 0) {
                bool present = false;
                for (ssize_t i = 0; i < count; ++i)
                    present |= devices[i] == libusb_get_device(handle);
                if (!present)
                    *reason = "device_unplugged";
                libusb_free_device_list(devices, 1);
            }
        }
        if (monitor_recorder) {
            bool complete = view_closed && ok && drained && restored && !run.sink_failed && !stream_failure;
            finish_monitor_recording(monitor, monitor_recorder, dump, &run, &clip_begin, stats->last_video_lock,
                                     complete ? "user_stopped" : *reason, complete);
        }
        raw_dump_finish_live(dump, stats);
    }
    return ok && drained && restored && !run.sink_failed && !stream_failure &&
           (view_closed || (seconds && elapsed >= seconds)) && !cx231xx_cancelled(cancel);
}

static bool route_capture(libusb_context *context, libusb_device_handle *handle, void *data)
{
    const struct capture_session *session = data;
    bool svideo = session->options->svideo;
    printf("  Routing %s NTSC-M with stereo line input.\n", svideo ? "S-Video" : "composite");
    return cx231xx_route_input(handle, svideo) &&
           cx231xx_with_stream_interfaces(context, handle, session->cancel, probe_stream, data,
                                        session->stats ? session->reason : NULL);
}

static bool signal_received(void *context)
{
    const struct capture_options *options = context;
    return options->signal_number && *options->signal_number;
}

int capture_run(const struct capture_options *options)
{
    const struct cx231xx_cancel cancel = {.requested = signal_received, .context = (void *)options};
    bool capture = options->stream || options->raw || options->matroska || options->view_only;
    uint8_t image[CX231XX_FIRMWARE_SIZE];
    if ((options->initialize || capture) && !cx231xx_read_firmware(options->firmware_path, image))
        return EXIT_FAILURE;

    libusb_context *context;
    int rc = libusb_init(&context);
    if (rc < 0) {
        usb_error("libusb_init", rc);
        return EXIT_FAILURE;
    }

    const struct libusb_version *version = libusb_get_version();
    printf("libusb %u.%u.%u.%u\n", version->major, version->minor, version->micro, version->nano);
    libusb_device **devices;
    ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        usb_error("libusb_get_device_list", (int)count);
        libusb_exit(context);
        return EXIT_FAILURE;
    }

    int found = 0;
    bool ok = true;
    struct raw_dump *dump = NULL;
    struct recorder *recorder = NULL;
    struct live_view *views[VIEW_COUNT] = {0};
    struct capture_stats stats = {0};
    const char *reason = "capture_setup_failed";
    for (ssize_t i = 0; i < count && !cx231xx_cancelled(&cancel); ++i) {
        struct libusb_device_descriptor desc;
        rc = libusb_get_device_descriptor(devices[i], &desc);
        if (rc < 0) {
            usb_error("libusb_get_device_descriptor", rc);
            continue;
        }
        if (desc.idVendor != HAUPPAUGE_VID || desc.idProduct != USB_LIVE2_PID)
            continue;

        ++found;
        describe_device(devices[i], &desc);
        for (int index = 0; index < desc.bNumConfigurations; ++index) {
            struct libusb_config_descriptor *config;
            rc = libusb_get_config_descriptor(devices[i], (uint8_t)index, &config);
            if (rc < 0) {
                usb_error("libusb_get_config_descriptor", rc);
                ok = false;
                continue;
            }
            describe_layout(config, libusb_get_device_speed(devices[i]));
            libusb_free_config_descriptor(config);
        }
        if (options->matroska) {
            recorder = recorder_open(options->directory, options->svideo, options->profile);
            if (!recorder) {
                ok = false;
                break;
            }
        }
        if (options->preview)
            views[VIEW_PREVIEW] = live_view_open(VIEW_PREVIEW);
        if (options->camera)
            views[VIEW_CAMERA] = live_view_open(VIEW_CAMERA);
        if (options->picture.terminal && !views[VIEW_PREVIEW]) {
            ok = false;
            break;
        }
        if (options->view_only && !options->monitor && !views[VIEW_PREVIEW] && !views[VIEW_CAMERA]) {
            ok = false;
            break;
        }
        if (options->matroska || options->view_only)
            dump = raw_dump_create_live(recorder, views);
        else if (options->raw)
            dump = raw_dump_create(options->seconds);
        if ((options->raw || options->matroska || options->view_only) && !dump) {
            fputs("Could not allocate the raw sample buffer.\n", stderr);
            ok = false;
            break;
        }
        if (options->monitor)
            raw_dump_attach_monitor(dump, options->monitor);
        if (options->claim || options->iso_test || options->initialize || capture) {
            struct capture_session session = {
                .options = options, .cancel = &cancel, .dump = dump, .reason = &reason,
                .stats = (recorder || options->view_only) ? &stats : NULL,
            };
            if (!cx231xx_usb_access(context, devices[i], &desc, options->iso_test,
                                    options->initialize || capture ? image : NULL, &cancel,
                                    capture ? route_capture : NULL, &session))
                ok = false;
        }
        // One output pair belongs to one device, even when several matching units are attached.
        if (options->raw || options->matroska || options->view_only)
            break;
    }
    if (found == 0)
        fprintf(stderr, "No Hauppauge USB-Live2 (%04x:%04x) visible to libusb.\n",
                HAUPPAUGE_VID, USB_LIVE2_PID);
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    if (recorder) {
        stats.signal = options->signal_number ? *options->signal_number : 0;
        if (stats.signal && (strcmp(reason, "capture_setup_failed") == 0 ||
                             strcmp(reason, "duration_reached") == 0))
            reason = "interrupted";
        if (!ok && strcmp(reason, "duration_reached") == 0)
            reason = "usb_cleanup_failed";
        ok = recorder_finish(recorder, reason, ok && !stats.signal, &stats);
    } else if (dump && options->raw) {
        ok = raw_dump_write(dump, options->directory, ok && !cx231xx_cancelled(&cancel));
    } else if (dump && !ok && !cx231xx_cancelled(&cancel)) {
        fprintf(stderr, "Live video stopped: %s\n", reason);
    }
    if (options->view_only && !cx231xx_cancelled(&cancel) && live_view_failed(views[VIEW_CAMERA]))
        ok = false;
    raw_dump_destroy(dump);
    for (size_t i = 0; i < VIEW_COUNT; ++i)
        live_view_close(views[i]);
    // A signal received after the recording stop was reported cannot change that outcome.
    return found > 0 && ok && (options->matroska || !cx231xx_cancelled(&cancel)) ? EXIT_SUCCESS : EXIT_FAILURE;
}
