// SPDX-License-Identifier: GPL-2.0-or-later
#define _DARWIN_C_SOURCE
#include "Fields.h"

#include <CoreMediaIO/CMIOHardware.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static CMIOObjectPropertyAddress property(CMIOObjectPropertySelector selector, CMIOObjectPropertyScope scope)
{
    return (CMIOObjectPropertyAddress){selector, scope, kCMIOObjectPropertyElementMain};
}

static CMIODeviceID find_camera(void)
{
    CMIOObjectPropertyAddress address = property(kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal);
    UInt32 size, used;
    if (CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &address, 0, NULL, &size) != noErr || !size)
        return 0;
    CMIODeviceID *devices = malloc(size);
    if (!devices)
        return 0;
    OSStatus status = CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &address, 0, NULL, size, &used, devices);
    CMIODeviceID result = 0;
    address.mSelector = kCMIODevicePropertyDeviceUID;
    if (status == noErr) {
        for (size_t i = 0; i < used / sizeof *devices; ++i) {
            CFStringRef uid = NULL;
            UInt32 uid_size;
            if (CMIOObjectGetPropertyData(devices[i], &address, 0, NULL, sizeof uid, &uid_size, &uid) != noErr || !uid)
                continue;
            if (CFStringCompare(uid, CFSTR("CC5F6F94-05AF-4F40-988F-BA728A2F76A9"),
                                kCFCompareCaseInsensitive) == kCFCompareEqualTo)
                result = devices[i];
            CFRelease(uid);
            if (result)
                break;
        }
    }
    free(devices);
    return result;
}

static CMIOStreamID find_sink(CMIODeviceID device)
{
    CMIOObjectPropertyAddress address = property(kCMIODevicePropertyStreams, kCMIODevicePropertyScopeOutput);
    CMIOStreamID sink = 0;
    UInt32 used;
    if (CMIOObjectGetPropertyData(device, &address, 0, NULL, sizeof sink, &used, &sink) != noErr ||
        used != sizeof sink)
        return 0;
    return sink;
}

static void queue_changed(CMIOStreamID stream, void *token, void *context)
{
    (void)stream;
    (void)token;
    (void)context;
}

static void publish_field(CMSimpleQueueRef queue, struct camera_fields *fields,
                          const uint8_t *raw, unsigned parity, CMTime pts)
{
    if (CMSimpleQueueGetFullness(queue) >= 1.0f)
        return;
    // CMIO takes the +1 reference on dequeue; the analyzer cannot model this opaque queue handoff.
    CMSampleBufferRef sample __attribute__((suppress)) = camera_make_field(fields, raw, parity, pts);
    // Transfer the sample reference only when the queue accepts it.
    if (sample && CMSimpleQueueEnqueue(queue, sample) != noErr)
        CFRelease(sample);
}

int main(int argc, char **argv)
{
    bool handoff = argc == 2 && strcmp(argv[1], "--usb-handoff") == 0;
    bool self_check = argc == 2 && strcmp(argv[1], "--check-fields") == 0;
    bool check_device = argc == 2 && strcmp(argv[1], "--check-device") == 0;
    if (argc != 1 && !self_check && !check_device && !handoff) {
        fputs("Usage: usblive2-camera-publish [--check-fields | --check-device]\n", stderr);
        return 2;
    }
    CMIODeviceID device = 0;
    CMIOStreamID sink = 0;
    CMSimpleQueueRef queue = NULL;
    OSStatus status;
    if (self_check) {
        status = CMSimpleQueueCreate(NULL, 4, &queue);
    } else {
        device = find_camera();
        sink = device ? find_sink(device) : 0;
        if (!sink) {
            fputs("USB-Live2 camera is unavailable. Activate USB-Live2 Camera in macOS first.\n", stderr);
            return EXIT_FAILURE;
        }
        if (check_device) {
            puts("USB-Live2 camera and publisher sink are visible to Core Media I/O.");
            return EXIT_SUCCESS;
        }
        status = CMIOStreamCopyBufferQueue(sink, queue_changed, NULL, &queue);
    }
    if (status != noErr || !queue) {
        fprintf(stderr, "Could not open USB-Live2 camera queue (%d).\n", status);
        return EXIT_FAILURE;
    }
    // The field self-check can run without access to the system IOSurface service.
    struct camera_fields *fields = camera_fields_create(!self_check);
    uint8_t *frame = malloc(NTSC_FRAME_BYTES);
    int result = EXIT_FAILURE;
    if (!fields || !frame) {
        fputs("Could not allocate USB-Live2 camera buffers.\n", stderr);
        goto cleanup;
    }
    if (self_check) {
        // Different flat fields make a missing, repeated, or blended field easy to detect.
        for (size_t row = 0; row < 2 * NTSC_FIELD_LINES; ++row) {
            uint8_t luma = row % 2 ? 235 : 16;
            for (size_t col = 0; col < NTSC_LINE_BYTES; ++col)
                frame[row * NTSC_LINE_BYTES + col] = col % 2 ? 128 : luma;
        }
        publish_field(queue, fields, frame, 0, CMTimeMake(0, 60000));
        publish_field(queue, fields, frame, 1, CMTimeMake(1001, 60000));
        bool matches = CMSimpleQueueGetCount(queue) == 2;
        for (unsigned field = 0; field < 2; ++field) {
            CMSampleBufferRef sample = (CMSampleBufferRef)CMSimpleQueueDequeue(queue);
            if (!sample) {
                matches = false;
                continue;
            }
            CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
            if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
                const uint8_t *pixel = CVPixelBufferGetBaseAddress(image);
                matches &= field ? pixel[0] > 240 : pixel[0] < 15;
                matches &= pixel[3] == 255;
                CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
            } else {
                matches = false;
            }
            matches &= CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample),
                                     CMTimeMake(1001 * field, 60000)) == 0;
            CFRelease(sample);
        }
        CMTime top, bottom, next_top, next_bottom;
        camera_field_times(fields, &top, &bottom);
        camera_field_times(fields, &next_top, &next_bottom);
        // A second pair observed immediately must still follow the first in host-clock order.
        matches &= CMTimeCompare(next_top, bottom) > 0;
        matches &= CMTimeCompare(CMTimeSubtract(next_bottom, next_top), CMTimeMake(1001, 60000)) == 0;
        puts(matches ? "Both camera fields produced distinct progressive frames." :
                       "Camera field conversion failed.");
        result = matches ? EXIT_SUCCESS : EXIT_FAILURE;
        goto cleanup;
    }
    status = CMIODeviceStartStream(device, sink);
    if (status != noErr) {
        fprintf(stderr, "Could not start USB-Live2 camera publishing (%d). Check the helper signature.\n", status);
        goto cleanup;
    }
    // The CLI must wait until the sink has released direct capture's USB claims.
    if (handoff && send(STDIN_FILENO, "R", 1, MSG_NOSIGNAL) != 1) {
        perror("Signal USB-Live2 publisher readiness");
        CMIODeviceStopStream(device, sink);
        goto cleanup;
    }
    while (fread(frame, 1, NTSC_FRAME_BYTES, stdin) == NTSC_FRAME_BYTES) {
        CMTime top, bottom;
        camera_field_times(fields, &top, &bottom);
        publish_field(queue, fields, frame, 0, top);
        publish_field(queue, fields, frame, 1, bottom);
    }
    result = ferror(stdin) ? EXIT_FAILURE : EXIT_SUCCESS;
    CMIODeviceStopStream(device, sink);

cleanup:
    free(frame);
    camera_fields_destroy(fields);
    if (sink) {
        CMSimpleQueueRef unregistered = NULL;
        CMIOStreamCopyBufferQueue(sink, NULL, NULL, &unregistered);
        if (unregistered)
            CFRelease(unregistered);
    }
    CFRelease(queue);
    return result;
}
