/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "cx231xx-init.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    uint8_t image[CX231XX_FIRMWARE_SIZE];
    // The default lookup must not depend on the working directory.
    assert(chdir("/tmp") == 0);
    assert(cx231xx_read_firmware(NULL, image));

    char path[] = "/tmp/usblive2-test-image.XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    memset(image, 0x5a, sizeof image);
    assert(write(file, image, sizeof image) == sizeof image);
    memset(image, 0, sizeof image);
    assert(cx231xx_read_firmware(path, image));
    assert(image[0] == 0x5a && image[sizeof image - 1] == 0x5a);
    assert(write(file, "x", 1) == 1);
    assert(!cx231xx_read_firmware(path, image));
    assert(close(file) == 0 && unlink(path) == 0);
    return 0;
}
