// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef USBLIVE2_CAMERA_CAPTURE_H
#define USBLIVE2_CAMERA_CAPTURE_H

#include <CoreMedia/CoreMedia.h>
#include <stdbool.h>

// Nullability is a deliberate Clang extension: Swift needs these callback and result contracts.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
/**
 * Claim USB and stream progressive samples until stopped or interrupted by a device or signal fault.
 * Call on a worker thread. Callbacks and their context are borrowed until USB cleanup completes.
 * Returns a static error string, or NULL after cancellation.
 */
const char *_Nullable camera_capture(const char *_Nonnull firmware,
                                     bool (*_Nonnull stopped)(void *_Nonnull),
                                     void (*_Nonnull deliver)(void *_Nonnull, CMSampleBufferRef _Nonnull),
                                     void *_Nonnull context);
#pragma clang diagnostic pop

#endif
