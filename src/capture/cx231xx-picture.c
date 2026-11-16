/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cx231xx-picture.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Native decoder steps, not percentages. Defaults reproduce the packed upstream luma/chroma words.
const struct picture_control picture_controls[] = {
    [PICTURE_BRIGHTNESS] = {"brightness", "Brightness", -128, 127, 18},
    [PICTURE_CONTRAST] = {"contrast", "Contrast", 0, 255, 125},
    [PICTURE_SATURATION] = {"saturation", "Saturation", 0, 255, 130},
    [PICTURE_HUE] = {"hue", "Hue", -128, 127, 0},
    [PICTURE_PEAKING] = {"peaking", "Luma peaking", 0, 4, 0},
    [PICTURE_LUMA_CORING] = {"luma-coring", "Luma coring", 0, 3, 0},
    [PICTURE_CHROMA_CORING] = {"chroma-coring", "Chroma coring", 0, 3, 1},
    [PICTURE_CHROMA_FILTER] = {"chroma-filter", "Chroma low-pass", 0, 1, 1},
    [PICTURE_CHROMA_DELAY] = {"chroma-delay", "Chroma delay", -4, 3, -1},
};

struct picture_settings picture_defaults(void)
{
    struct picture_settings defaults;
    for (int i = 0; i < PICTURE_CONTROL_COUNT; ++i)
        defaults.value[i] = picture_controls[i].initial;
    return defaults;
}

bool picture_parse_setting(struct picture_settings *settings, const char *arg)
{
    const char *equals = strchr(arg, '=');
    if (!equals || !equals[1])
        return false;

    char *end;
    errno = 0;
    long value = strtol(equals + 1, &end, 10);
    if (errno || *end || end == equals + 1)
        return false;

    size_t length = (size_t)(equals - arg);
    for (int i = 0; i < PICTURE_CONTROL_COUNT; ++i) {
        const struct picture_control *control = &picture_controls[i];
        if (strlen(control->name) != length || strncmp(control->name, arg, length))
            continue;
        if (value < control->min || value > control->max)
            return false;
        settings->value[i] = (int)value;
        return true;
    }
    return false;
}

bool picture_encode(const struct picture_settings *settings, uint32_t *luma, uint32_t *chroma)
{
    // Validate the entire adjustment before replacing any register fields.
    const int *value = settings->value;
    for (int i = 0; i < PICTURE_CONTROL_COUNT; ++i)
        if (value[i] < picture_controls[i].min || value[i] > picture_controls[i].max)
            return false;

    // CX231xx has a separate peaking enable bit, so selector zero is UI level one.
    uint32_t peaking = 0;
    if (value[PICTURE_PEAKING])
        peaking = 0x40000u | ((uint32_t)(value[PICTURE_PEAKING] - 1) << 16);
    *luma = (*luma & ~UINT32_C(0x00c7ffff)) | peaking |
        (uint8_t)value[PICTURE_BRIGHTNESS] | ((uint32_t)value[PICTURE_CONTRAST] << 8) |
        ((uint32_t)value[PICTURE_LUMA_CORING] << 22);

    // Keep both chroma gains equal. Delay is a signed three-bit value.
    uint32_t saturation = (uint32_t)value[PICTURE_SATURATION];
    uint32_t delay = (uint32_t)value[PICTURE_CHROMA_DELAY] & 7u;
    *chroma = (*chroma & ~UINT32_C(0x3fffffff)) | saturation | (saturation << 8) |
        ((uint32_t)(uint8_t)value[PICTURE_HUE] << 16) | ((uint32_t)value[PICTURE_CHROMA_CORING] << 24) |
        (delay << 26) | ((uint32_t)value[PICTURE_CHROMA_FILTER] << 29);
    return true;
}
