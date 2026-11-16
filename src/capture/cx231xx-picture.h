/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CX231XX_PICTURE_H
#define CX231XX_PICTURE_H

#include <stdbool.h>
#include <stdint.h>

enum picture_control_id {
    PICTURE_BRIGHTNESS, PICTURE_CONTRAST, PICTURE_SATURATION, PICTURE_HUE,
    PICTURE_PEAKING, PICTURE_LUMA_CORING, PICTURE_CHROMA_CORING,
    PICTURE_CHROMA_FILTER, PICTURE_CHROMA_DELAY, PICTURE_CONTROL_COUNT,
};

/** Inclusive bounds and startup value in native register steps, shared by the CLI and app. */
struct picture_control {
    const char *name;
    const char *label;
    int min, max, initial;
};

extern const struct picture_control picture_controls[PICTURE_CONTROL_COUNT];

/** An entire picture adjustment. Array positions are picture_control_id values. */
struct picture_settings {
    int value[PICTURE_CONTROL_COUNT];
};

enum {
    PICTURE_LUMA_DEFAULT = 0x00107d12,
    PICTURE_CHROMA_DEFAULT = 0x3d008282,
};

struct picture_settings picture_defaults(void);

/** Parse one name=value pair. Invalid arguments leave the settings intact. */
bool picture_parse_setting(struct picture_settings *settings, const char *arg);

/** Merge documented picture fields into existing decoder words. Failure leaves both intact. */
bool picture_encode(const struct picture_settings *settings, uint32_t *luma, uint32_t *chroma);

#endif
