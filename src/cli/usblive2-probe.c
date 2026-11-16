// SPDX-License-Identifier: GPL-2.0-or-later
#define _DARWIN_C_SOURCE
#include "cx231xx-session.h"
#include "cx231xx-raw.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { DEFAULT_SECONDS = 600, MAX_SECONDS = 3600 };

static volatile sig_atomic_t interrupted;

static void interrupt_probe(int signal_number)
{
    interrupted = signal_number;
}

static void print_usage(FILE *file, const char *program)
{
    fprintf(file, "Usage: %s [OPTIONS] COMMAND\n"
            "  --claim | --iso-test | --init\n"
            "  --stream INPUT [SECONDS]\n"
            "  --dump INPUT SECONDS NEW_DIRECTORY\n"
            "  --record INPUT [SECONDS] NEW_DIRECTORY\n"
            "  --view INPUT  (requires --preview or --camera)\n"
            "INPUT is svideo or composite. With no command, list connected devices.\n"
            "Options before COMMAND:\n"
            "  --firmware PATH  Override the bundled Linux firmware\n"
            "  --picture NAME=VALUE  Set a picture control (repeatable)\n"
            "  --codec ffv1|prores|h264|h264-hw|raw  Recording output (default ffv1)\n"
            "  --preview | --camera  Enable live outputs\n"
            "  --live-controls  Use Terminal controls with --preview --view\n"
            "Use --picture-help for ranges and defaults.\n", program);
}

static bool capture_input(const char *arg)
{
    return strcmp(arg, "svideo") == 0 || strcmp(arg, "composite") == 0;
}

int main(int argc, char **argv)
{
    const char *program = argv[0];
    bool preview = false, camera = false;
    struct picture_session picture = {.initial = picture_defaults()};
    if (argc == 2 && strcmp(argv[1], "--picture-help") == 0) {
        print_picture_controls();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(stdout, program);
        return 0;
    }
    const char *firmware_path = NULL;
    bool profile_set = false;
    enum capture_profile profile = CAPTURE_FFV1;
    // Consume leading options so the existing positional capture commands remain compatible.
    while (argc > 1) {
        if (!preview && strcmp(argv[1], "--preview") == 0)
            preview = true;
        else if (!camera && strcmp(argv[1], "--camera") == 0)
            camera = true;
        else if (!picture.terminal && (strcmp(argv[1], "--live-controls") == 0 ||
                                       strcmp(argv[1], "--live-peaking") == 0))
            picture.terminal = true;
        else if (!firmware_path && argc > 2 && strcmp(argv[1], "--firmware") == 0) {
            firmware_path = argv[2];
            --argc;
            ++argv;
        } else if (argc > 2 && strcmp(argv[1], "--picture") == 0) {
            if (!picture_parse_setting(&picture.initial, argv[2])) {
                fprintf(stderr, "Invalid picture setting '%s'. See --picture-help.\n", argv[2]);
                return 2;
            }
            picture.configured = true;
            --argc;
            ++argv;
        }
        else if (!profile_set && argc > 2 && strcmp(argv[1], "--codec") == 0) {
            for (; profile < CAPTURE_PROFILE_COUNT; ++profile)
                if (strcmp(argv[2], capture_profile_name(profile)) == 0)
                    break;
            if (profile == CAPTURE_PROFILE_COUNT) {
                fputs("Codec must be ffv1, prores, h264, h264-hw, or raw.\n", stderr);
                return 2;
            }
            profile_set = true;
            --argc;
            ++argv;
        }
        else
            break;
        --argc;
        ++argv;
    }
    const char *mode = argc > 1 ? argv[1] : "";
    bool claim = argc == 2 && strcmp(mode, "--claim") == 0;
    bool iso_test = argc == 2 && strcmp(mode, "--iso-test") == 0;
    bool initialize = strcmp(mode, "--init") == 0;
    bool stream = strcmp(mode, "--stream") == 0;
    bool matroska = strcmp(mode, "--record") == 0;
    bool viewing = preview || camera;
    bool view_only = viewing && strcmp(mode, "--view") == 0;
    bool raw = strcmp(mode, "--dump") == 0;
    bool capture = stream || raw || matroska || view_only;
    int input = 2;
    // Accept the old firmware-first syntax when the following argument is a valid input.
    if (!firmware_path && ((initialize && argc == 3) ||
        (capture && argc >= 4 && !capture_input(argv[2]) && capture_input(argv[3]))))
        firmware_path = argv[input++];
    int operands = argc - input;
    if ((profile_set && !matroska) || (picture.configured && !capture) ||
        (firmware_path && ((!initialize && !capture) || !*firmware_path)) ||
        (viewing && !matroska && !view_only) ||
        (argc != 1 && !claim && !iso_test && !initialize && !capture) ||
        (initialize && operands != 0) || (stream && (operands < 1 || operands > 2)) ||
        (view_only && operands != 1) || (matroska && (operands < 2 || operands > 3)) ||
        (raw && operands != 3) || (capture && !capture_input(argv[input]))) {
        print_usage(stderr, program);
        return 2;
    }
    bool svideo = capture && strcmp(argv[input], "svideo") == 0;
    // Live changes are for a preview session with keyboard input.
    if (picture.terminal && (!preview || !view_only || !isatty(STDIN_FILENO))) {
        fputs("--live-controls requires --preview --view and an interactive terminal.\n", stderr);
        return 2;
    }
    unsigned seconds = 0;
    if (capture) {
        seconds = (matroska || view_only) ? 0 : DEFAULT_SECONDS;
        if (operands == 3 || (stream && operands == 2)) {
            unsigned maximum = MAX_SECONDS;
            if (raw)
                maximum = RAW_MAX_SECONDS;
            if (matroska)
                maximum = UINT_MAX;
            char *end;
            errno = 0;
            long value = strtol(argv[input + 1], &end, 10);
            if (errno || *end || value < 1 || (unsigned long)value > maximum) {
                fprintf(stderr, "Duration must be an integer from 1 to %u seconds.\n", maximum);
                return 2;
            }
            seconds = (unsigned)value;
        }
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    if ((iso_test || initialize || capture) && (signal(SIGINT, interrupt_probe) == SIG_ERR ||
                                                signal(SIGTERM, interrupt_probe) == SIG_ERR)) {
        perror("signal");
        return EXIT_FAILURE;
    }
    const struct capture_options options = {
        .firmware_path = firmware_path, .directory = argv[argc - 1], .seconds = seconds,
        .profile = profile, .claim = claim, .iso_test = iso_test, .initialize = initialize,
        .stream = stream, .view_only = view_only, .raw = raw, .matroska = matroska,
        .svideo = svideo, .preview = preview, .camera = camera, .picture = picture,
        .signal_number = &interrupted,
    };
    return capture_run(&options);
}
