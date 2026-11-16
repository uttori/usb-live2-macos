// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef USBLIVE2_CAMERA_FIELDS_H
#define USBLIVE2_CAMERA_FIELDS_H

#include <CoreMedia/CoreMedia.h>
#include "cx231xx-video.h"

struct camera_fields;

/** Allocate converter and pixel pool for one producer thread. shared enables IOSurface backing. */
struct camera_fields *camera_fields_create(bool shared);
void camera_fields_destroy(struct camera_fields *fields);

/** Produce successive host-clock presentation times at 60000/1001 fields per second. */
void camera_field_times(struct camera_fields *fields, CMTime *top, CMTime *bottom);

/** Make an owned progressive sample from one parity of a woven YUYV frame. NULL means drop it. */
CMSampleBufferRef camera_make_field(struct camera_fields *fields, const uint8_t *raw,
                                    unsigned parity, CMTime pts) CF_RETURNS_RETAINED;

#endif
