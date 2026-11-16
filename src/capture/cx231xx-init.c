// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * USB-Live2 initialization adapted from Linux v6.12 (adc218676eef25575469234709c2d87185ca223a).
 * cx231xx bridge, I2C, PCB and AFE code: Copyright (C) 2008 Srinivasa Deevi, Conexant.
 * cx25840 decoder code: Copyright (C) 2004 Ulf Eklund, based on the saa7115 driver and
 * Chris Kennedy's cx25840 driver, with V4L2 changes by Tyler Trafford.
 * Firmware protocol: Linux cx25840-firmware.c, with transfer sizing work by Mike Isely.
 * Register names/bit fields: drivers/media/usb/cx231xx/cx231xx-reg.h in that revision.
 * Keep the packed upstream presets and their write order: these are board calibration,
 * not user preferences. Picture controls are the supported adjustment surface.
 */
#define _POSIX_C_SOURCE 200809L
#include "cx231xx-init.h"

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    CONTROL_TIMEOUT_MS = 1000,
    GET_REGISTER = 0x0d,
    SET_REGISTER = 0x0c,
    SET_GPIO = 0x08,
    FOUR_BYTE_MASK = 0x0f, // Enable all four byte lanes of a bridge register.
    BOARD_CFG_STAT = 0x00,
    TS_MODE_REG = 0x04,
    EP_MODE_SET = 0x18,
    EP_AUDIO = 0x04,
    EP_VIDEO = 0x08,
    PWR_CTL_EN = 0x74,
    PWR_MODE_MASK = 0x17f,
    PWR_AV_EN = 0x08,
    PWR_ISO_EN = 0x40,
    PWR_EXTERNAL_AV = 0x30,
    // Seven-bit addresses on bridge I²C master 0; distinct from decoder register offsets.
    AFE = 0x30,
    DECODER = 0x44,
    I2S = 0x4c,
    AFE_POWER = 0x08,
    AFE_PLL_ON = 0x18,
    DOWNLOAD_LOW = 0x800,
    DOWNLOAD_HIGH = 0x801,
    DOWNLOAD_DATA = 0x802,
    DOWNLOAD_CONTROL = 0x803,
    DOWNLOAD_ENABLE = 0x08,
    MCU_RUN = 0x10,
    FIRMWARE_CHUNK = 14, // 16-byte bridge FIFO minus the two-byte decoder address.
};

/** Resolve shipped data from the real executable so the working directory and symlinks do not matter. */
static bool bundled_firmware(char path[static PATH_MAX])
{
    char executable[PATH_MAX], resolved[PATH_MAX];
    uint32_t length = sizeof executable;
    if (_NSGetExecutablePath(executable, &length) != 0 || !realpath(executable, resolved))
        return false;
    *strrchr(resolved, '/') = '\0';
    const char *locations[] = {"firmware", "../Resources/firmware", "../firmware"};
    for (size_t i = 0; i < sizeof locations / sizeof locations[0]; ++i) {
        int count = snprintf(path, PATH_MAX, "%s/%s/v4l-cx231xx-avcore-01.fw", resolved, locations[i]);
        if (count > 0 && count < PATH_MAX && access(path, R_OK) == 0)
            return true;
    }
    return false;
}

bool cx231xx_read_firmware(const char *path, uint8_t image[static CX231XX_FIRMWARE_SIZE])
{
    char shipped[PATH_MAX];
    if (!path) {
        if (!bundled_firmware(shipped)) {
            fputs("Bundled Linux firmware is missing. Keep the firmware directory with the capture program or select a firmware image.\n", stderr);
            return false;
        }
        path = shipped;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return false;
    }
    size_t length = fread(image, 1, CX231XX_FIRMWARE_SIZE, file);
    bool exact = length == CX231XX_FIRMWARE_SIZE && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!exact) {
        fprintf(stderr, "%s: expected exactly %u readable firmware bytes.\n", path, CX231XX_FIRMWARE_SIZE);
        return false;
    }
    printf("  Firmware image: %s\n", path);
    return true;
}

/** Reject short control transfers, including partial writes that cannot safely be resumed. */
static bool control(libusb_device_handle *usb, bool read, uint8_t request, uint16_t value,
                    uint16_t index, uint8_t *data, uint16_t size)
{
    uint8_t direction = read ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT;
    int rc = libusb_control_transfer(usb, direction | LIBUSB_REQUEST_TYPE_VENDOR, request,
                                     value, index, data, size, CONTROL_TIMEOUT_MS);
    if (rc == size)
        return true;
    fprintf(stderr, "Vendor %s request 0x%02x value 0x%04x index 0x%04x: ",
            read ? "IN" : "OUT", request, value, index);
    if (rc < 0)
        fprintf(stderr, "%s (%d)\n", libusb_error_name(rc), rc);
    else
        fprintf(stderr, "short transfer (%d of %u bytes)\n", rc, size);
    return false;
}

static uint32_t from_le(const uint8_t *bytes, unsigned size)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint32_t)bytes[i] << (i * 8);
    return value;
}

static void to_le(uint8_t *bytes, unsigned size, uint32_t value)
{
    for (unsigned i = 0; i < size; ++i)
        bytes[i] = (uint8_t)(value >> (i * 8));
}

static bool bridge_read(libusb_device_handle *usb, uint16_t reg, uint32_t *value)
{
    uint8_t bytes[4];
    if (!control(usb, true, GET_REGISTER, FOUR_BYTE_MASK, reg, bytes, sizeof bytes))
        return false;
    *value = from_le(bytes, sizeof bytes);
    return true;
}

static bool bridge_write(libusb_device_handle *usb, uint16_t reg, uint32_t value)
{
    uint8_t bytes[4];
    to_le(bytes, sizeof bytes, value);
    return control(usb, false, SET_REGISTER, FOUR_BYTE_MASK, reg, bytes, sizeof bytes);
}

/** Read 1–4 little-endian bytes from a seven-bit device address on I²C master 0. */
static bool i2c_read(libusb_device_handle *usb, uint8_t chip, uint16_t reg, uint8_t size, uint32_t *value)
{
    // wValue packs chip[15:9], 100 kHz/synchronous mode (0x21), and subaddress width[3:2].
    unsigned subaddress_size = chip == I2S ? 1 : 2;
    uint16_t setup = (uint16_t)(((unsigned)chip << 9) | 0x21u | (subaddress_size << 2));
    uint8_t bytes[4];
    if (!control(usb, true, 4, setup, reg, bytes, size))
        return false;
    *value = from_le(bytes, size);
    return true;
}

/** Write the low 1–4 bytes of value; callers supply a register width from the pinned Linux sequence. */
static bool i2c_write(libusb_device_handle *usb, uint8_t chip, uint16_t reg, uint8_t size, uint32_t value)
{
    uint8_t bytes[6];
    // cx25840 sends its register address inside the I²C payload. AFE and I²S use the bridge's subaddress.
    if (chip == DECODER) {
        bytes[0] = reg >> 8;
        bytes[1] = reg & 0xff;
        to_le(bytes + 2, size, value);
        return control(usb, false, 0, (uint16_t)((chip << 9) | 0x21), 0, bytes, size + 2);
    }
    unsigned subaddress_size = chip == I2S ? 1 : 2;
    to_le(bytes, size, value);
    return control(usb, false, 0, (uint16_t)(((unsigned)chip << 9) | 0x21u | (subaddress_size << 2)), reg, bytes, size);
}

/** Preserve register bits outside the clear/set masks. */
static bool i2c_update(libusb_device_handle *usb, uint8_t chip, uint16_t reg, uint8_t size,
                       uint32_t clear, uint32_t set)
{
    uint32_t value;
    return i2c_read(usb, chip, reg, size, &value) &&
           i2c_write(usb, chip, reg, size, (value & ~clear) | set);
}

static void settle_ms(long milliseconds)
{
    struct timespec delay = {.tv_nsec = milliseconds * 1000000};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
        // An interrupted delay still owes the hardware its remaining settling time.
    }
}

static bool enable_afe_pll(libusb_device_handle *usb)
{
    // Bound the Linux power latch poll so a stuck AFE produces a useful failure.
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        uint32_t status;
        if (!i2c_write(usb, AFE, AFE_POWER, 1, AFE_PLL_ON) ||
            !i2c_read(usb, AFE, AFE_POWER, 1, &status))
            return false;
        if (status == AFE_PLL_ON)
            return true;
    }
    fputs("AFE PLL enable did not latch.\n", stderr);
    return false;
}

static bool power_external_av(libusb_device_handle *usb, uint32_t initial)
{
    // Bring AV, isochronous, and external-input power up separately with Linux's 10 ms settling gaps.
    uint32_t power = (initial & ~(uint32_t)PWR_MODE_MASK) | PWR_AV_EN;
    if (!bridge_write(usb, PWR_CTL_EN, power))
        return false;
    settle_ms(10);
    power |= PWR_ISO_EN;
    if (!bridge_write(usb, PWR_CTL_EN, power))
        return false;
    settle_ms(10);
    power |= PWR_EXTERNAL_AV;
    if (!bridge_write(usb, PWR_CTL_EN, power))
        return false;
    settle_ms(10);
    uint32_t actual;
    if (!enable_afe_pll(usb) ||
        !i2c_write(usb, AFE, 0x23, 1, 0) ||
        !i2c_write(usb, AFE, 0x43, 1, 0) ||
        !i2c_write(usb, AFE, 0x63, 1, 0) ||
        !i2c_write(usb, I2S, 0x0f, 1, 0) ||
        !bridge_read(usb, PWR_CTL_EN, &actual))
        return false;
    printf("  Bridge power 0x%08x -> 0x%08x\n", initial, actual);
    if ((actual & PWR_MODE_MASK) == (power & PWR_MODE_MASK))
        return true;
    fputs("Bridge did not latch external A/V power.\n", stderr);
    return false;
}

static bool tune_afe(libusb_device_handle *usb)
{
    uint32_t tune;
    // Split the 0x23c reference count across the tuning registers before starting the filter.
    if (!i2c_write(usb, AFE, 0x01, 1, 0x3c) ||
        !i2c_read(usb, AFE, 0x01, 1, &tune) ||
        !i2c_write(usb, AFE, 0x00, 1, 0x42) ||
        !i2c_write(usb, AFE, 0x05, 1, 0x0f) ||
        !enable_afe_pll(usb) ||
        !i2c_write(usb, AFE, 0x02, 1, 0x40))
        return false;
    settle_ms(5);
    return i2c_write(usb, AFE, 0x02, 1, 0);
}

static bool init_afe(libusb_device_handle *usb)
{
    if (!tune_afe(usb) ||
        !i2c_write(usb, AFE, 0x23, 1, 0) ||
        !i2c_write(usb, AFE, 0x43, 1, 0) ||
        !i2c_write(usb, AFE, 0x63, 1, 0) ||
        !i2c_write(usb, AFE, 0x0b, 1, 2))
        return false;

    // Three ADC banks are spaced 0x20 apart; reset, calibrate for 5 ms, release, then pre-clamp.
    const struct { uint8_t reg, value, delay_ms; } calibration[] = {
        {0x27, 0x17, 0}, {0x22, 0x10, 5}, {0x27, 0x07, 0}, {0x29, 0xf0, 0},
    };
    for (size_t step = 0; step < sizeof calibration / sizeof calibration[0]; ++step) {
        for (unsigned channel = 0; channel < 3; ++channel)
            if (!i2c_write(usb, AFE, (uint16_t)(calibration[step].reg + 0x20 * channel), 1, calibration[step].value))
                return false;
        if (calibration[step].delay_ms)
            settle_ms(calibration[step].delay_ms);
    }
    // Clear termination/resistor bits for diode operation, then select the calibrated input clamp.
    for (unsigned channel = 0; channel < 3; ++channel)
        if (!i2c_update(usb, AFE, (uint16_t)(0x2a + 0x20 * channel), 1, 0xf8, 0))
            return false;
    for (unsigned channel = 0; channel < 3; ++channel)
        if (!i2c_write(usb, AFE, (uint16_t)(0x26 + 0x20 * channel), 1, 3))
            return false;
    return true;
}

/** Decoder writes are issued in table order, preserving upstream transfer widths. */
struct decoder_write {
    uint16_t reg;
    uint8_t size;
    uint32_t value;
};

static bool decoder_write_all(libusb_device_handle *usb, const struct decoder_write *writes, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (!i2c_write(usb, DECODER, writes[i].reg, writes[i].size, writes[i].value))
            return false;
    return true;
}

static bool reset_decoder(libusb_device_handle *usb)
{
    // Upstream analog-only reset presets: stop DLL, select clocks/DIF bypass, then restore picture words.
    const struct decoder_write defaults[] = {
        {0x398, 1, 0}, {0x002, 1, 0x76}, {0x001, 1, 0x40},
        {0x33c, 4, 1}, {0x340, 4, 0x0df7df83},
        {0x414, 4, PICTURE_LUMA_DEFAULT}, {0x420, 4, PICTURE_CHROMA_DEFAULT},
        {0x102, 1, 0x10}, {0x103, 1, 0x11}, {0x400, 1, 0}, {0x401, 1, 0xe8},
    };
    // Reset the decoder and stop the MCU before changing clocks or writing its program RAM.
    return i2c_update(usb, DECODER, 0x102, 1, 1, 1) &&
           i2c_update(usb, DECODER, 0x102, 1, 1, 0) &&
           i2c_update(usb, DECODER, DOWNLOAD_CONTROL, 1, MCU_RUN, 0) &&
           decoder_write_all(usb, defaults, sizeof defaults / sizeof defaults[0]);
}

static bool upload_firmware(libusb_device_handle *usb, const uint8_t *image,
                            const struct cx231xx_cancel *cancel)
{
    const struct decoder_write start[] = {
        {DOWNLOAD_LOW, 1, 0}, {DOWNLOAD_HIGH, 1, 0}, {DOWNLOAD_CONTROL, 1, 0x0b}, {0x000, 1, 0x20},
    };
    bool ok = decoder_write_all(usb, start, sizeof start / sizeof start[0]);
    size_t offset = 0;
    // The bridge accepts at most 16 I²C bytes, including the two-byte FIFO register prefix.
    while (ok && !cx231xx_cancelled(cancel) && offset < CX231XX_FIRMWARE_SIZE) {
        size_t count = CX231XX_FIRMWARE_SIZE - offset;
        if (count > FIRMWARE_CHUNK)
            count = FIRMWARE_CHUNK;
        uint8_t packet[2 + FIRMWARE_CHUNK] = {DOWNLOAD_DATA >> 8, DOWNLOAD_DATA & 0xff};
        memcpy(packet + 2, image + offset, count);
        ok = control(usb, false, 0, (DECODER << 9) | 0x21, 0, packet, (uint16_t)(count + 2));
        if (ok)
            offset += count;
    }

    // Clear download mode even after a short write or interruption. The MCU stays stopped.
    bool auto_increment = i2c_write(usb, DECODER, 0x000, 1, 0);
    bool download_disabled = i2c_write(usb, DECODER, DOWNLOAD_CONTROL, 1, 0x03);
    if (!ok || cx231xx_cancelled(cancel) || !auto_increment || !download_disabled) {
        fprintf(stderr, "Firmware upload stopped at %zu of %u bytes.\n", offset, CX231XX_FIRMWARE_SIZE);
        return false;
    }
    uint32_t high, low;
    if (!i2c_read(usb, DECODER, DOWNLOAD_HIGH, 1, &high) ||
        !i2c_read(usb, DECODER, DOWNLOAD_LOW, 1, &low))
        return false;
    unsigned loaded = (high << 8) | low;
    printf("  Decoder download counter: %u bytes\n", loaded);
    if (loaded == CX231XX_FIRMWARE_SIZE)
        return true;
    fputs("Decoder did not acknowledge the complete firmware image.\n", stderr);
    return false;
}

static bool set_ntsc_timing(libusb_device_handle *usb)
{
    // The decoder's NTSC-M defaults precede the bridge's capture timing overrides.
    // 0x470–477 pack horizontal/vertical counts; 0x478–47f set comb filtering and chroma timing.
    const struct decoder_write timing[] = {
        {0x49f, 1, 0x14},
        {0x470, 1, 0x7a}, {0x471, 1, 0x00}, {0x472, 1, 0x2d}, {0x473, 1, 0x5b},
        {0x474, 1, 0x1a}, {0x475, 1, 0x70}, {0x476, 1, 0x1e}, {0x477, 1, 0x1a},
        {0x478, 1, 0x1f}, {0x479, 1, 0x02}, {0x47a, 1, 0x50}, {0x47b, 1, 0x66},
        {0x47c, 1, 0x1f}, {0x47d, 1, 0x7c}, {0x47e, 1, 0x08}, {0x47f, 1, 0},
    };
    return decoder_write_all(usb, timing, sizeof timing / sizeof timing[0]);
}

/** Apply the cx25840 video subdevice's routing and input-change sequence. */
static bool decoder_route(libusb_device_handle *usb, uint8_t mux, bool svideo)
{
    const struct decoder_write audio[] = {
        {0x8d3, 1, 0x1f}, {0x8d0, 4, 0x1f063870},
        {0x900, 4, 0x08014faa}, {0x904, 4, 0x08014faa}, {0x90c, 4, 0x08014faa},
    };
    const struct decoder_write finish[] = {
        {0x124, 1, 0x03}, {0x914, 1, 0xa0}, {0x918, 1, 0xa0}, {0x919, 1, 1},
    };
    return i2c_write(usb, DECODER, 0x103, 1, mux) &&
        i2c_update(usb, DECODER, 0x401, 1, 0x06, svideo ? 2 : 0) &&
        // The board's two external inputs both use ADC2 channel 2.
        i2c_update(usb, DECODER, 0x102, 1, 0x04, 0) &&
        i2c_update(usb, DECODER, 0x102, 1, 0x02, 0) &&
        i2c_update(usb, DECODER, 0x102, 1, 0x02, 0) &&
        i2c_update(usb, DECODER, 0x810, 1, 1, 1) &&
        i2c_update(usb, DECODER, DOWNLOAD_CONTROL, 1, MCU_RUN, 0) &&
        decoder_write_all(usb, audio, sizeof audio / sizeof audio[0]) &&
        i2c_update(usb, DECODER, DOWNLOAD_CONTROL, 1, MCU_RUN, MCU_RUN) &&
        i2c_update(usb, DECODER, 0x810, 1, 1, 0) &&
        i2c_update(usb, DECODER, DOWNLOAD_CONTROL, 1, MCU_RUN, MCU_RUN) &&
        i2c_write(usb, DECODER, 0x402, 1, 0x04) &&
        i2c_write(usb, DECODER, 0x49f, 1, 0x14) &&
        i2c_update(usb, DECODER, 0x401, 1, 0x60, 0) &&
        i2c_update(usb, DECODER, 0x401, 1, 0x60, 0x60) &&
        i2c_update(usb, DECODER, 0x810, 1, 1, 1) &&
        i2c_write(usb, DECODER, 0x808, 1, 0xf6) &&
        i2c_write(usb, DECODER, 0x80b, 1, 0) &&
        i2c_update(usb, DECODER, 0x810, 1, 1, 0) &&
        decoder_write_all(usb, finish, sizeof finish / sizeof finish[0]);
}

static bool start_decoder(libusb_device_handle *usb)
{
    // Enable raw closed-caption/VBI framing before applying the final capture window in route_input.
    const struct decoder_write cc[] = {
        {0x404, 1, 0x0b}, {0x42f, 1, 0x66}, {0x474, 4, 0x1e1e601a},
    };
    if (!set_ntsc_timing(usb) || !decoder_route(usb, 0xf6, false) ||
        !i2c_update(usb, DECODER, DOWNLOAD_CONTROL, 1, MCU_RUN, MCU_RUN) ||
        !decoder_write_all(usb, cc, sizeof cc / sizeof cc[0]))
        return false;

    uint32_t status;
    if (!i2c_read(usb, DECODER, DOWNLOAD_CONTROL, 1, &status))
        return false;
    printf("  Decoder download control: 0x%02x\n", status);
    if ((status & (MCU_RUN | DOWNLOAD_ENABLE)) == MCU_RUN)
        return true;
    fputs("Decoder MCU run bit did not latch or download mode is still set.\n", stderr);
    return false;
}

static bool prepare_decoder(libusb_device_handle *usb)
{
    // Baseband bypass, I²S clock and board pin setup precede decoder identification.
    if (!i2c_write(usb, DECODER, 0x340, 4, 0x0df7df83) ||
        !i2c_update(usb, DECODER, 0x33c, 4, 0, 0x80000000) ||
        !i2c_update(usb, I2S, 0x0e, 1, 0, 0x80) ||
        !i2c_write(usb, I2S, 0x0f, 1, 0) ||
        !i2c_update(usb, DECODER, 0x120, 4, 0, 0x3b))
        return false;
    uint32_t high, low;
    if (!i2c_read(usb, DECODER, 0x101, 1, &high) ||
        !i2c_read(usb, DECODER, 0x100, 1, &low))
        return false;
    unsigned id = (high << 8) | low;
    printf("  Decoder ID: 0x%04x\n", id);
    if ((id & 0xfff0) == 0x5a30 && id != 0x5a3c)
        return reset_decoder(usb);
    fputs("Unexpected CX231xx decoder ID.\n", stderr);
    return false;
}

bool cx231xx_init_board(libusb_device_handle *usb,
                       const uint8_t image[static CX231XX_FIRMWARE_SIZE],
                       const struct cx231xx_cancel *cancel)
{
    uint32_t pcb, power, endpoints;
    if (!bridge_read(usb, BOARD_CFG_STAT, &pcb) ||
        !bridge_read(usb, PWR_CTL_EN, &power) ||
        !bridge_read(usb, EP_MODE_SET, &endpoints))
        return false;
    printf("  PCB configuration: 0x%08x, USB endpoint enables: 0x%08x\n", pcb, endpoints);
    // Linux's bus-powered analog-only scenario plus the bridge's high-speed flag.
    if ((pcb & 0xc5) != 0xc1 || (endpoints & 0x0c) != 0) {
        fputs("Initialization requires the high-speed analog-only PCB with idle A/V endpoints.\n", stderr);
        return false;
    }
    if (cx231xx_cancelled(cancel))
        return false;

    // Match Linux's initial GPIO state: GPIO 0 is an output with the cached output value zero.
    uint8_t gpio[4] = {0};
    bool prepared = control(usb, false, SET_GPIO, 0, 1, gpio, sizeof gpio) &&
                    power_external_av(usb, power) && init_afe(usb) && !cx231xx_cancelled(cancel) &&
                    prepare_decoder(usb);
    bool uploaded = prepared && !cx231xx_cancelled(cancel) && upload_firmware(usb, image, cancel);
    if (!uploaded || cx231xx_cancelled(cancel) || !start_decoder(usb) || cx231xx_cancelled(cancel)) {
        // A failure after upload can occur after the MCU has already started.
        if (uploaded && !i2c_update(usb, DECODER, DOWNLOAD_CONTROL, 1, MCU_RUN, 0))
            fputs("Could not stop the decoder MCU.\n", stderr);
        // After a partial initialization, restore bridge power if the device is still reachable.
        if (!bridge_write(usb, PWR_CTL_EN, power))
            fputs("Could not restore bridge power.\n", stderr);
        fputs("Board initialization incomplete. Power-cycle the USB-Live2 before retrying.\n", stderr);
        return false;
    }
    puts("  Board initialized and decoder firmware running.");
    return true;
}

static bool route_video(libusb_device_handle *usb, bool svideo)
{
    // Both inputs use the single-ended AFE reference. S-Video also routes its chroma channel.
    if (!tune_afe(usb) || !i2c_update(usb, AFE, 0x28, 1, 0x30, svideo ? 0 : 0x10))
        return false;
    if (svideo && !i2c_update(usb, AFE, 0x48, 1, 0x30, 0))
        return false;
    if (!i2c_update(usb, DECODER, 0x104, 4, svideo ? 0x01ff8000 : 0x01ff8020, 0x01000010))
        return false;
    // Linux sets VIP 1.1 and blue field on its initial composite route, then retains them for S-Video.
    if (!i2c_update(usb, DECODER, 0x404, 4, 3, 0x82))
        return false;
    if (!i2c_write(usb, DECODER, 0x340, 4, 0x0df7df83) ||
        !i2c_update(usb, DECODER, 0x33c, 4, 0, 0x80000000) ||
        !i2c_update(usb, DECODER, 0x488, 4, 0, 0x18000000) ||
        !i2c_update(usb, DECODER, 0x400, 4, 0x600, svideo ? 0x220 : 0x020))
        return false;
    if (svideo && (!i2c_update(usb, DECODER, 0x104, 4, 0x180, 0x1000) ||
                   !i2c_update(usb, AFE, 0x43, 1, 0x50, 0)))
        return false;

    uint32_t output;
    if (!i2c_read(usb, DECODER, 0x404, 4, &output))
        return false;
    // Linux enables raw VBI framing and VIP blanking when the current output is VIP.
    output |= 0x08;
    if (output & 0x02)
        output |= 0x80000;
    return i2c_write(usb, DECODER, 0x404, 4, output) &&
           decoder_route(usb, svideo ? 1 : 2, svideo);
}

static bool route_line_audio(libusb_device_handle *usb)
{
    // Linux holds the audio path in soft reset while switching to the board's line input.
    if (!i2c_write(usb, I2S, 0x0f, 1, 0) ||
        !i2c_write(usb, I2S, 0x0e, 1, 0x80) ||
        !i2c_update(usb, DECODER, 0x810, 1, 1, 1))
        return false;
    bool ok = i2c_write(usb, DECODER, 0x124, 4, 0) &&
              i2c_update(usb, DECODER, 0x91c, 4, 0, 0x01000000) &&
              i2c_write(usb, DECODER, 0x910, 4, 0) &&
              i2c_write(usb, DECODER, 0x800, 4, 0x03000001) &&
              i2c_write(usb, DECODER, 0x8d0, 4, 0x00063073) &&
              i2c_update(usb, DECODER, 0x8d4, 4, 0, 0x7fff0000) &&
              i2c_update(usb, DECODER, 0x8dc, 4, 0, 0x7fff0000);
    return i2c_update(usb, DECODER, 0x810, 1, 1, 0) && ok;
}

bool cx231xx_route_input(libusb_device_handle *usb, bool svideo)
{
    if (!route_video(usb, svideo) || !route_line_audio(usb))
        return false;
    // Selecting a standard resets timing defaults. Apply Linux's NTSC capture overrides afterward.
    // DFE_CTRL3 reduces VHS flagging; VERT_TIM_CTRL uses 24 blank, 487 active, 28 BT.656 blank lines.
    // HORIZ_TIM_CTRL moves active video to count 121; the parser crops each field to 240 rows.
    return i2c_update(usb, DECODER, 0x400, 1, 0x0f, 1) &&
           i2c_update(usb, DECODER, 0x403, 1, 3, 0) &&
           set_ntsc_timing(usb) &&
           i2c_write(usb, DECODER, 0x490, 4, 0xcd3f0280) &&
           i2c_update(usb, DECODER, 0x474, 4, 0xff3ff3ff, 0x1c1e7018) &&
           i2c_update(usb, DECODER, 0x470, 4, 0x3ff, 0x79);
}

static bool write_picture_word(libusb_device_handle *usb, uint16_t reg, uint32_t expected)
{
    uint32_t after;
    if (!i2c_write(usb, DECODER, reg, 4, expected) || !i2c_read(usb, DECODER, reg, 4, &after))
        return false;
    if (after != expected) {
        fprintf(stderr, "Decoder register 0x%03x read back 0x%08x, expected 0x%08x.\n", reg, after, expected);
        return false;
    }
    return true;
}

bool cx231xx_set_picture(libusb_device_handle *usb, const struct picture_settings *settings)
{
    enum { LUMA_CTRL = 0x414, CHROMA_CTRL = 0x420 };
    uint32_t original_luma, original_chroma;
    if (!i2c_read(usb, DECODER, LUMA_CTRL, 4, &original_luma) ||
        !i2c_read(usb, DECODER, CHROMA_CTRL, 4, &original_chroma))
        return false;
    uint32_t luma = original_luma, chroma = original_chroma;
    if (!picture_encode(settings, &luma, &chroma))
        return false;

    // A failed request may already have changed the hardware. Restore each attempted word.
    bool luma_changed = luma != original_luma;
    bool luma_ok = !luma_changed || write_picture_word(usb, LUMA_CTRL, luma);
    bool chroma_attempted = luma_ok && chroma != original_chroma;
    if (luma_ok && (!chroma_attempted || write_picture_word(usb, CHROMA_CTRL, chroma))) {
        printf("  Picture controls applied (luma 0x%08x, chroma 0x%08x).\n", luma, chroma);
        return true;
    }
    if (luma_changed && !write_picture_word(usb, LUMA_CTRL, original_luma))
        fputs("Could not restore previous luma controls.\n", stderr);
    if (chroma_attempted && !write_picture_word(usb, CHROMA_CTRL, original_chroma))
        fputs("Could not restore previous chroma controls.\n", stderr);
    return false;
}

bool cx231xx_read_stream_state(libusb_device_handle *usb, struct cx231xx_stream_state *state)
{
    return bridge_read(usb, TS_MODE_REG, &state->mode) &&
           bridge_read(usb, EP_MODE_SET, &state->endpoints);
}

bool cx231xx_set_streams(libusb_device_handle *usb, const struct cx231xx_stream_state *saved,
                         bool enable)
{
    if (!enable) {
        // Attempt both restores even if clearing the endpoints fails.
        bool endpoints = bridge_write(usb, EP_MODE_SET, saved->endpoints);
        if (!bridge_write(usb, TS_MODE_REG, saved->mode) || !endpoints)
            return false;
        struct cx231xx_stream_state actual;
        if (!cx231xx_read_stream_state(usb, &actual))
            return false;
        if (actual.endpoints != saved->endpoints || actual.mode != saved->mode) {
            fprintf(stderr, "USB stream restore mismatch: mode 0x%08x, endpoints 0x%08x.\n",
                    actual.mode, actual.endpoints);
            return false;
        }
        return true;
    }
    // Linux starts raw video first, then adds audio. Its audio mode retains the raw video mode bit.
    return bridge_write(usb, TS_MODE_REG, 0x0100) &&
           bridge_write(usb, EP_MODE_SET, saved->endpoints | EP_VIDEO) &&
           bridge_write(usb, TS_MODE_REG, 0x9300) &&
           bridge_write(usb, EP_MODE_SET, saved->endpoints | EP_VIDEO | EP_AUDIO);
}

bool cx231xx_video_status(libusb_device_handle *usb, uint32_t *status)
{
    return i2c_read(usb, DECODER, 0x40c, 4, status);
}
