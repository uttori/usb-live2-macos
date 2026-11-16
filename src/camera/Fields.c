// SPDX-License-Identifier: GPL-2.0-or-later
#include "Fields.h"

#include <Accelerate/Accelerate.h>
#include <stdlib.h>

// Square-pixel 4:3 output for camera clients; the captured 720×480 raster uses 8:9 pixels.
enum { WIDTH = 640, HEIGHT = 480 };

/** Reuses conversion storage and caps the number of frames retained by downstream clients. */
struct camera_fields {
    CVPixelBufferPoolRef pool;
    CFDictionaryRef options;
    CMTime previous;
    uint8_t rgb[NTSC_WIDTH * NTSC_FIELD_LINES * 4];
};

static uint8_t byte_clip(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

static void convert_pair(const uint8_t *yuyv, uint8_t *bgra)
{
    // Integer BT.601 limited-range YCbCr to full-range BGRA, scaled by 256.
    int cb = (int)yuyv[1] - 128, cr = (int)yuyv[3] - 128;
    for (unsigned i = 0; i < 2; ++i) {
        int y = 298 * ((int)yuyv[2 * i] - 16);
        bgra[4 * i] = byte_clip((y + 516 * cb + 128) / 256);
        bgra[4 * i + 1] = byte_clip((y - 100 * cb - 208 * cr + 128) / 256);
        bgra[4 * i + 2] = byte_clip((y + 409 * cr + 128) / 256);
        bgra[4 * i + 3] = 255;
    }
}

CMSampleBufferRef camera_make_field(struct camera_fields *fields, const uint8_t *raw,
                                    unsigned parity, CMTime pts)
{
    CVPixelBufferRef pixel = NULL;
    CVReturn status = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(NULL, fields->pool, fields->options, &pixel);
    if (status != kCVReturnSuccess)
        return NULL;
    if (CVPixelBufferLockBaseAddress(pixel, 0) != kCVReturnSuccess) {
        CFRelease(pixel);
        return NULL;
    }
    // Even and odd rows carry consecutive instants. Scale one parity at a time.
    for (size_t row = 0; row < NTSC_FIELD_LINES; ++row) {
        const uint8_t *source = raw + (2 * row + parity) * NTSC_LINE_BYTES;
        uint8_t *dest = fields->rgb + row * NTSC_WIDTH * 4;
        for (size_t column = 0; column < NTSC_LINE_BYTES; column += 4)
            convert_pair(source + column, dest + 2 * column);
    }
    vImage_Buffer rgb = {fields->rgb, NTSC_FIELD_LINES, NTSC_WIDTH, NTSC_WIDTH * 4};
    vImage_Buffer output = {CVPixelBufferGetBaseAddress(pixel), HEIGHT, WIDTH,
                            CVPixelBufferGetBytesPerRow(pixel)};
    vImage_Error converted = vImageScale_ARGB8888(&rgb, &output, NULL, kvImageNoFlags);
    CVPixelBufferUnlockBaseAddress(pixel, 0);
    CMSampleBufferRef sample = NULL;
    CMVideoFormatDescriptionRef format = NULL;
    if (converted == kvImageNoError &&
        CMVideoFormatDescriptionCreateForImageBuffer(NULL, pixel, &format) == noErr) {
        CMSampleTimingInfo timing = {
            .duration = CMTimeMake(1001, 60000),
            .presentationTimeStamp = pts,
            .decodeTimeStamp = kCMTimeInvalid,
        };
        CMSampleBufferCreateReadyWithImageBuffer(NULL, pixel, format, &timing, &sample);
        CFRelease(format);
    }
    CFRelease(pixel);
    return sample;
}

/** Bound the number of frames that can remain retained by consumers. */
static CFDictionaryRef pool_limit(void)
{
    int ceiling = 8;
    CFNumberRef limit = CFNumberCreate(NULL, kCFNumberIntType, &ceiling);
    if (!limit)
        return NULL;
    const void *keys[] = {kCVPixelBufferPoolAllocationThresholdKey};
    const void *values[] = {limit};
    CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1,
                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(limit);
    return options;
}

static CVPixelBufferPoolRef make_pool(bool shared)
{
    int pixel_format = kCVPixelFormatType_32BGRA, width = WIDTH, height = HEIGHT;
    CFNumberRef numbers[] = {
        CFNumberCreate(NULL, kCFNumberIntType, &pixel_format),
        CFNumberCreate(NULL, kCFNumberIntType, &width),
        CFNumberCreate(NULL, kCFNumberIntType, &height),
    };
    CVPixelBufferPoolRef pool = NULL;
    if (numbers[0] && numbers[1] && numbers[2]) {
        // IOSurface bridges this dictionary through Objective-C, even when it is empty.
        CFDictionaryRef surface = CFDictionaryCreate(NULL, NULL, NULL, 0,
                                  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void *keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey,
                              kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey};
        const void *values[] = {numbers[0], numbers[1], numbers[2], surface};
        CFIndex attribute_count = shared ? 4 : 3;
        CFDictionaryRef attributes = surface ? CFDictionaryCreate(NULL, keys, values, attribute_count,
                                      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
        if (attributes) {
            CVPixelBufferPoolCreate(NULL, NULL, attributes, &pool);
            CFRelease(attributes);
        }
        if (surface)
            CFRelease(surface);
    }
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i)
        if (numbers[i])
            CFRelease(numbers[i]);
    return pool;
}

struct camera_fields *camera_fields_create(bool shared)
{
    struct camera_fields *fields = malloc(sizeof *fields);
    if (!fields)
        return NULL;
    fields->pool = make_pool(shared);
    fields->options = pool_limit();
    fields->previous = kCMTimeInvalid;
    if (!fields->pool || !fields->options) {
        camera_fields_destroy(fields);
        return NULL;
    }
    return fields;
}

void camera_fields_destroy(struct camera_fields *fields)
{
    if (!fields)
        return;
    if (fields->options)
        CFRelease(fields->options);
    if (fields->pool)
        CFRelease(fields->pool);
    free(fields);
}

void camera_field_times(struct camera_fields *fields, CMTime *top, CMTime *bottom)
{
    const CMTime duration = CMTimeMake(1001, 60000);
    CMTime now = CMTimeConvertScale(CMClockGetTime(CMClockGetHostTimeClock()), duration.timescale,
                                   kCMTimeRoundingMethod_Default);
    *top = CMTimeSubtract(now, duration);
    // Completions may bunch up after a scheduler delay. Keep successive pairs in display order.
    if (CMTIME_IS_VALID(fields->previous) && CMTimeCompare(*top, fields->previous) <= 0)
        *top = CMTimeAdd(fields->previous, duration);
    *bottom = fields->previous = CMTimeAdd(*top, duration);
}
