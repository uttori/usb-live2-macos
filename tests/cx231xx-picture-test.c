/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cx231xx-picture.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct picture_settings settings = picture_defaults();
    uint32_t luma = PICTURE_LUMA_DEFAULT, chroma = PICTURE_CHROMA_DEFAULT;
    assert(picture_encode(&settings, &luma, &chroma));
    assert(luma == PICTURE_LUMA_DEFAULT && chroma == PICTURE_CHROMA_DEFAULT);
    for (int i = 0; i < PICTURE_CONTROL_COUNT; ++i) {
        const struct picture_control *control = &picture_controls[i];
        char argument[64];
        snprintf(argument, sizeof argument, "%s=%d", control->name, control->min);
        assert(picture_parse_setting(&settings, argument) && settings.value[i] == control->min);
        snprintf(argument, sizeof argument, "%s=%d", control->name, control->min - 1);
        assert(!picture_parse_setting(&settings, argument) && settings.value[i] == control->min);
        snprintf(argument, sizeof argument, "%s=%d", control->name, control->max);
        assert(picture_parse_setting(&settings, argument) && settings.value[i] == control->max);
        snprintf(argument, sizeof argument, "%s=%d", control->name, control->max + 1);
        assert(!picture_parse_setting(&settings, argument) && settings.value[i] == control->max);
    }
    assert(!picture_parse_setting(&settings, "hue=12.5"));
    assert(!picture_parse_setting(&settings, "hue="));
    assert(!picture_parse_setting(&settings, "peaking=9223372036854775808"));

    // Exercise signed fields, paired chroma gain, and preservation of unrelated high bits.
    assert(picture_parse_setting(&settings, "brightness=-128"));
    assert(picture_parse_setting(&settings, "hue=-1"));
    assert(picture_parse_setting(&settings, "chroma-delay=-4"));
    luma |= 0x80000000u;
    chroma |= 0xc0000000u;
    assert(picture_encode(&settings, &luma, &chroma));
    assert(luma == 0x80d7ff80u && chroma == 0xf3ffffffu);
    settings.value[PICTURE_PEAKING] = 5;
    assert(!picture_encode(&settings, &luma, &chroma));
    assert(luma == 0x80d7ff80u && chroma == 0xf3ffffffu);
    return 0;
}
