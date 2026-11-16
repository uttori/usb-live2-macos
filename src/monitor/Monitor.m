// SPDX-License-Identifier: GPL-2.0-or-later
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <os/lock.h>
#include <math.h>

#include "cx231xx-monitor.h"
#include "cx231xx-raw.h"
#include "cx231xx-record.h"
#include "cx231xx-session.h"

#ifdef USBLIVE2_CAMERA_APP
#import "Installer.h"
#endif

/** Owns the native controls and preview. Only the capture worker calls the USB backend. */
@interface MonitorController : NSObject <NSApplicationDelegate>
@end

@implementation MonitorController {
#ifdef USBLIVE2_CAMERA_APP
    CameraInstaller *_installer;
#endif
    NSWindow *_window;
    NSPopUpButton *_input;
    NSButton *_firmwareButton, *_startButton, *_muteButton, *_recordButton, *_resetButton;
    NSTextField *_status, *_recordStatus;
    NSMutableArray<NSSlider *> *_sliders;
    NSMutableArray<NSTextField *> *_values;
    NSURL *_firmware;
    BOOL _running, _previewReady, _quitting, _audioUnavailable;
    NSURL *_recordDirectory;
    dispatch_queue_t _recordQueue;

    // Transfer the prepared recorder and stop flags without holding the lock during disk or USB I/O.
    os_unfair_lock _settingsLock;
    struct picture_settings _desired;
    bool _stopping;
    bool _stopRecording;
    struct recorder *_pendingRecorder;

    AVSampleBufferDisplayLayer *_display;
    AVSampleBufferVideoRenderer *_renderer;
    CVPixelBufferPoolRef _pixels;
    dispatch_queue_t _videoQueue, _audioQueue;
    dispatch_semaphore_t _videoSlot, _audioSlots;
    uint8_t _frame[NTSC_FRAME_BYTES];
    BOOL _firstFrame;

    AVAudioEngine *_engine;
    AVAudioPlayerNode *_player;
    AVAudioFormat *_audioFormat;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    _settingsLock = OS_UNFAIR_LOCK_INIT;
    _desired = picture_defaults();
    _videoQueue = dispatch_queue_create("video.usblive2.monitor", DISPATCH_QUEUE_SERIAL);
    _audioQueue = dispatch_queue_create("audio.usblive2.monitor", DISPATCH_QUEUE_SERIAL);
    _recordQueue = dispatch_queue_create("record.usblive2.monitor", DISPATCH_QUEUE_SERIAL);
    _videoSlot = dispatch_semaphore_create(1);

    NSString *saved = [NSUserDefaults.standardUserDefaults stringForKey:@"FirmwarePath"];
    if (saved && [NSFileManager.defaultManager isReadableFileAtPath:saved])
        _firmware = [NSURL fileURLWithPath:saved];

    [self buildWindow];
    [self showSettings:_desired];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activate];
}

- (void)buildWindow
{
    NSString *appName = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleName"];
    NSMenu *menuBar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [menuBar addItem:appItem];
    appItem.submenu = [[NSMenu alloc] initWithTitle:appName];
#ifdef USBLIVE2_CAMERA_APP
    // Install the optional extension from the menu so opening the app goes straight to capture.
    _installer = [CameraInstaller new];
    NSMenuItem *install = [appItem.submenu addItemWithTitle:@"Install Camera Extension…"
                                                    action:@selector(activate:) keyEquivalent:@""];
    install.target = _installer;
    [appItem.submenu addItem:NSMenuItem.separatorItem];
#endif
    [appItem.submenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                              action:@selector(terminate:) keyEquivalent:@"q"];
    NSApp.mainMenu = menuBar;

    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1002, 599)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered defer:NO];
    _window.title = appName;
    _window.releasedWhenClosed = NO;
    [_window center];

    _input = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_input addItemsWithTitles:@[@"S-Video", @"Composite"]];
    [_input setAccessibilityLabel:@"Video input"];
    _firmwareButton = [NSButton buttonWithTitle:@"Firmware…" target:self action:@selector(chooseFirmware:)];
    _firmwareButton.toolTip = _firmware ? _firmware.path : @"Using bundled Linux firmware";
    _startButton = [NSButton buttonWithTitle:@"Start Preview" target:self action:@selector(togglePreview:)];
    _recordButton = [NSButton buttonWithTitle:@"Record…" target:self action:@selector(toggleRecording:)];
    _recordButton.enabled = NO;
    _muteButton = [NSButton checkboxWithTitle:@"Mute" target:self action:@selector(muteAudio:)];
    NSStackView *toolbar = [NSStackView stackViewWithViews:@[_input, _firmwareButton, _startButton,
                                                           _recordButton, _muteButton]];
    toolbar.spacing = 10;

    NSView *video = [NSView new];
    video.wantsLayer = YES;
    _display = [AVSampleBufferDisplayLayer layer];
    // The view itself is 4:3, which corrects the NTSC sample aspect without a rescale in capture.
    _display.videoGravity = AVLayerVideoGravityResize;
    _display.backgroundColor = NSColor.blackColor.CGColor;
    _renderer = _display.sampleBufferRenderer;
    video.layer = _display;
    [video.widthAnchor constraintEqualToConstant:640].active = YES;
    [video.heightAnchor constraintEqualToConstant:480].active = YES;
    NSDictionary *pixelAttributes = @{
        (id)kCVPixelBufferWidthKey: @(NTSC_WIDTH),
        (id)kCVPixelBufferHeightKey: @(NTSC_FIELD_LINES * 2),
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_422YpCbCr8),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (__bridge CFDictionaryRef)pixelAttributes, &_pixels);

    _sliders = [NSMutableArray array];
    _values = [NSMutableArray array];
    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    for (int i = 0; i < PICTURE_CONTROL_COUNT; ++i) {
        const struct picture_control *control = &picture_controls[i];
        NSString *name = @(control->label);
        NSTextField *label = [NSTextField labelWithString:name];
        NSSlider *slider = [NSSlider sliderWithValue:control->initial minValue:control->min
                                          maxValue:control->max target:self action:@selector(changeControl:)];
        slider.tag = i;
        slider.continuous = YES;
        if (control->max - control->min <= 7) {
            slider.numberOfTickMarks = control->max - control->min + 1;
            slider.allowsTickMarkValuesOnly = YES;
        }
        slider.toolTip = [NSString stringWithFormat:@"%d to %d. Default %d.", control->min, control->max, control->initial];
        [slider setAccessibilityLabel:name];
        [slider setAccessibilityHelp:slider.toolTip];
        [slider.widthAnchor constraintEqualToConstant:152].active = YES;
        NSTextField *value = [NSTextField labelWithString:@""];
        value.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        value.alignment = NSTextAlignmentRight;
        [value.widthAnchor constraintEqualToConstant:35].active = YES;
        [_sliders addObject:slider];
        [_values addObject:value];
        [rows addObject:@[label, slider, value]];
    }
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    for (NSInteger row = 0; row < grid.numberOfRows; ++row)
        [grid rowAtIndex:row].height = 26;
    _resetButton = [NSButton buttonWithTitle:@"Reset Defaults" target:self action:@selector(resetPicture:)];
    // Pin the controls to the top so the preview's height cannot stretch one grid row.
    NSView *controls = [NSView new];
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    _resetButton.translatesAutoresizingMaskIntoConstraints = NO;
    [controls addSubview:grid];
    [controls addSubview:_resetButton];
    [NSLayoutConstraint activateConstraints:@[
        [controls.heightAnchor constraintEqualToConstant:480],
        [grid.leadingAnchor constraintEqualToAnchor:controls.leadingAnchor],
        [grid.trailingAnchor constraintEqualToAnchor:controls.trailingAnchor],
        [grid.topAnchor constraintEqualToAnchor:controls.topAnchor],
        [_resetButton.leadingAnchor constraintEqualToAnchor:controls.leadingAnchor],
        [_resetButton.topAnchor constraintEqualToAnchor:grid.bottomAnchor constant:16],
    ]];

    NSStackView *body = [NSStackView stackViewWithViews:@[video, controls]];
    body.alignment = NSLayoutAttributeTop;
    body.spacing = 18;

    _status = [NSTextField labelWithString:@"Connect the USB-Live2, then start preview."];
    _status.textColor = NSColor.secondaryLabelColor;
    _recordStatus = [NSTextField labelWithString:@""];
    _recordStatus.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [_recordStatus.widthAnchor constraintLessThanOrEqualToConstant:960].active = YES;
    NSStackView *layout = [NSStackView stackViewWithViews:@[toolbar, body, _status, _recordStatus]];
    layout.orientation = NSUserInterfaceLayoutOrientationVertical;
    layout.alignment = NSLayoutAttributeLeading;
    layout.spacing = 12;
    layout.translatesAutoresizingMaskIntoConstraints = NO;
    [_window.contentView addSubview:layout];
    [NSLayoutConstraint activateConstraints:@[
        [layout.topAnchor constraintEqualToAnchor:_window.contentView.topAnchor constant:14],
        [layout.leadingAnchor constraintEqualToAnchor:_window.contentView.leadingAnchor constant:14],
    ]];
}

- (void)showSettings:(struct picture_settings)settings
{
    for (NSUInteger i = 0; i < PICTURE_CONTROL_COUNT; ++i) {
        _sliders[i].intValue = settings.value[i];
        _values[i].stringValue = [NSString stringWithFormat:@"%d", settings.value[i]];
    }
}

- (void)changeControl:(NSSlider *)sender
{
    NSUInteger index = (NSUInteger)sender.tag;
    const struct picture_control *control = &picture_controls[index];
    int value = (int)lround(sender.doubleValue);
    value = MAX(control->min, MIN(control->max, value));
    os_unfair_lock_lock(&_settingsLock);
    _desired.value[index] = value;
    os_unfair_lock_unlock(&_settingsLock);
    _values[index].stringValue = [NSString stringWithFormat:@"%d", value];
}

- (void)resetPicture:(id)sender
{
    (void)sender;
    struct picture_settings defaults = picture_defaults();
    os_unfair_lock_lock(&_settingsLock);
    _desired = defaults;
    os_unfair_lock_unlock(&_settingsLock);
    [self showSettings:defaults];
}

- (void)toggleRecording:(id)sender
{
    (void)sender;
    if (_recordDirectory) {
        os_unfair_lock_lock(&_settingsLock);
        _stopRecording = true;
        os_unfair_lock_unlock(&_settingsLock);
        _recordButton.enabled = NO;
        _recordStatus.stringValue = @"Finishing recording…";
        return;
    }
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;
    panel.prompt = @"Record Here";
    panel.message = @"Choose where to create a new USB-Live2 capture folder.";
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse result) {
        if (result == NSModalResponseOK && self->_previewReady && self->_startButton.enabled) {
            NSDateFormatter *date = [NSDateFormatter new];
            date.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
            date.dateFormat = @"yyyy-MM-dd HHmmss";
            NSString *name = [@"Capture " stringByAppendingString:[date stringFromDate:NSDate.date]];
            [self beginRecording:[panel.URL URLByAppendingPathComponent:name isDirectory:YES]];
        }
    }];
}

- (void)beginRecording:(NSURL *)directory
{
    _recordDirectory = directory;
    _recordButton.enabled = NO;
    _recordStatus.stringValue = @"Preparing recording…";
    // Keep one verified set of decoder settings for the duration of the clip.
    _resetButton.enabled = NO;
    for (NSSlider *slider in _sliders)
        slider.enabled = NO;
    BOOL svideo = _input.indexOfSelectedItem == 0;
    dispatch_async(_recordQueue, ^{
        struct recorder *recorder = recorder_open(directory.fileSystemRepresentation, svideo, CAPTURE_FFV1);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!recorder) {
                [self recordingFinished:@"Could not create the capture. Choose a writable folder for a new capture."];
                return;
            }
            if (!self->_running || self->_stopping) {
                const struct capture_stats stats = {0};
                [self finishRecorder:recorder stats:&stats reason:"preview_ended" complete:NO];
                return;
            }
            os_unfair_lock_lock(&self->_settingsLock);
            self->_pendingRecorder = recorder;
            self->_stopRecording = false;
            os_unfair_lock_unlock(&self->_settingsLock);
            self->_recordButton.enabled = YES;
            self->_recordButton.title = @"Stop Recording";
            self->_recordStatus.stringValue = [@"Recording to " stringByAppendingString:directory.path];
        });
    });
}

/** Pass the prepared recorder to the USB thread exactly once. */
- (struct recorder *)takeRecorder:(bool *)stop
{
    os_unfair_lock_lock(&_settingsLock);
    struct recorder *pending = _pendingRecorder;
    _pendingRecorder = NULL;
    *stop = _stopRecording;
    os_unfair_lock_unlock(&_settingsLock);
    return pending;
}

- (void)finishRecorder:(struct recorder *)recorder stats:(const struct capture_stats *)stats
                reason:(const char *)reason complete:(bool)complete
{
    struct capture_stats clip = *stats;
    dispatch_async(_recordQueue, ^{
        bool saved = recorder_finish(recorder, reason, complete, &clip);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString *path = self->_recordDirectory.path;
            NSString *message = saved ? [@"Saved " stringByAppendingString:[path stringByAppendingPathComponent:@"capture.mkv"]] :
                [@"Recording incomplete. See the capture report in " stringByAppendingString:path];
            [self recordingFinished:message];
        });
    });
}

- (void)recordingFinished:(NSString *)message
{
    _recordDirectory = nil;
    _recordStatus.stringValue = message;
    _recordStatus.toolTip = message;
    _recordButton.title = @"Record…";
    _recordButton.enabled = _previewReady && !_stopping;
    _startButton.enabled = !_running || !_stopping;
    _resetButton.enabled = YES;
    for (NSSlider *slider in _sliders)
        slider.enabled = YES;
    if (_quitting && !_running)
        [NSApp replyToApplicationShouldTerminate:YES];
}

- (void)chooseFirmware:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @"Choose the USB-Live2 firmware image.";
    panel.directoryURL = _firmware.URLByDeletingLastPathComponent;
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse result) {
        if (result == NSModalResponseOK) {
            self->_firmware = panel.URL;
            self->_firmwareButton.toolTip = panel.URL.path;
            [NSUserDefaults.standardUserDefaults setObject:panel.URL.path forKey:@"FirmwarePath"];
            self->_status.stringValue = @"Firmware selected. Start preview to connect.";
        }
    }];
}

- (void)muteAudio:(NSButton *)sender
{
    float volume = sender.state == NSControlStateValueOn ? 0.0f : 1.0f;
    AVAudioPlayerNode *player = _player;
    dispatch_async(_audioQueue, ^{ player.volume = volume; });
}

- (void)startAudio
{
    _audioUnavailable = NO;
    _audioSlots = dispatch_semaphore_create(5);
    _audioFormat = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
    _engine = [AVAudioEngine new];
    _player = [AVAudioPlayerNode new];
    [_engine attachNode:_player];
    [_engine connect:_player to:_engine.mainMixerNode format:_audioFormat];
    NSError *error = nil;
    if (![_engine startAndReturnError:&error]) {
        NSLog(@"USB-Live2 audio output unavailable: %@", error);
        _audioUnavailable = YES;
        _player = nil;
        _engine = nil;
        return;
    }
    _player.volume = _muteButton.state == NSControlStateValueOn ? 0.0f : 1.0f;
    [_player play];
}

/** Queue no more than five PCM blocks so the preview cannot accumulate audio latency. */
- (void)offerAudio:(const uint8_t *)pcm size:(size_t)size
{
    AVAudioPlayerNode *player = _player;
    dispatch_semaphore_t slots = _audioSlots;
    if (!player || !size || size % PCM_FRAME_BYTES || dispatch_semaphore_wait(slots, DISPATCH_TIME_NOW))
        return;
    NSData *copy = [NSData dataWithBytes:pcm length:size];
    dispatch_async(_audioQueue, ^{
        AVAudioFrameCount frames = (AVAudioFrameCount)(copy.length / PCM_FRAME_BYTES);
        AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:self->_audioFormat frameCapacity:frames];
        if (!buffer) {
            dispatch_semaphore_signal(slots);
            return;
        }
        buffer.frameLength = frames;
        const int16_t *samples = copy.bytes;
        float *left = buffer.floatChannelData[0];
        float *right = buffer.floatChannelData[1];
        for (AVAudioFrameCount i = 0; i < frames; ++i) {
            left[i] = samples[i * 2] / 32768.0f;
            right[i] = samples[i * 2 + 1] / 32768.0f;
        }
        [player scheduleBuffer:buffer completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
             completionHandler:^(AVAudioPlayerNodeCompletionCallbackType type) {
            (void)type;
            dispatch_semaphore_signal(slots);
        }];
    });
}

/** Prepare a display-only top-field preview without delaying the USB event thread. */
- (void)offerVideo:(const uint8_t *)frame
{
    if (dispatch_semaphore_wait(_videoSlot, DISPATCH_TIME_NOW))
        return;
    memcpy(_frame, frame, sizeof _frame);
    dispatch_async(_videoQueue, ^{
        [self displayFrame];
        dispatch_semaphore_signal(self->_videoSlot);
    });
}

- (void)displayFrame
{
    if (!_renderer.readyForMoreMediaData || !_pixels)
        return;

    CVPixelBufferRef pixels = NULL;
    NSDictionary *limit = @{(id)kCVPixelBufferPoolAllocationThresholdKey: @4};
    if (CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, _pixels,
             (__bridge CFDictionaryRef)limit, &pixels) != kCVReturnSuccess)
        return;
    if (CVPixelBufferLockBaseAddress(pixels, 0) != kCVReturnSuccess) {
        CFRelease(pixels);
        return;
    }
    uint8_t *base = CVPixelBufferGetBaseAddress(pixels);
    size_t stride = CVPixelBufferGetBytesPerRow(pixels);
    for (unsigned row = 0; row < 2 * NTSC_FIELD_LINES; ++row) {
        // Line doubling avoids blending adjacent fields. Core Video expects UYVY byte order.
        const uint8_t *src = _frame + (row & ~1u) * NTSC_LINE_BYTES;
        uint8_t *dst = base + row * stride;
        for (unsigned i = 0; i < NTSC_LINE_BYTES; i += 2) {
            dst[i] = src[i + 1];
            dst[i + 1] = src[i];
        }
    }
    CVPixelBufferUnlockBaseAddress(pixels, 0);
    CVBufferSetAttachment(pixels, kCVImageBufferYCbCrMatrixKey,
                          kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);

    CMVideoFormatDescriptionRef format = NULL;
    CMSampleBufferRef sample = NULL;
    CMSampleTimingInfo timing = {kCMTimeInvalid, kCMTimeZero, kCMTimeInvalid};
    if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixels, &format) == noErr &&
        CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pixels, format, &timing, &sample) == noErr) {
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
        CFMutableDictionaryRef flags = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFDictionarySetValue(flags, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
        [_renderer enqueueSampleBuffer:sample];
        CFRelease(sample);
        if (!_firstFrame) {
            _firstFrame = YES;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self->_running && self->_startButton.enabled) {
                    self->_previewReady = YES;
                    self->_recordButton.enabled = self->_recordDirectory == nil;
                    self->_status.stringValue = self->_audioUnavailable ? @"Live · audio output unavailable" : @"Live";
                }
            });
        }
    }
    if (format)
        CFRelease(format);
    CFRelease(pixels);
}

- (bool)copySettings:(struct picture_settings *)settings
{
    os_unfair_lock_lock(&_settingsLock);
    *settings = _desired;
    bool running = !_stopping;
    os_unfair_lock_unlock(&_settingsLock);
    return running;
}

static void monitor_video(void *context, const uint8_t *frame)
{
    @autoreleasepool {
        [(__bridge MonitorController *)context offerVideo:frame];
    }
}

static void monitor_audio(void *context, const uint8_t *pcm, size_t size)
{
    @autoreleasepool {
        [(__bridge MonitorController *)context offerAudio:pcm size:size];
    }
}

static bool monitor_settings(void *context, struct picture_settings *settings)
{
    return [(__bridge MonitorController *)context copySettings:settings];
}

static struct recorder *monitor_recording(void *context, bool *stop)
{
    return [(__bridge MonitorController *)context takeRecorder:stop];
}

static void monitor_finish_recording(void *context, struct recorder *recorder,
                                     const struct capture_stats *stats, const char *reason, bool complete)
{
    [(__bridge MonitorController *)context finishRecorder:recorder stats:stats reason:reason complete:complete];
}

- (void)togglePreview:(id)sender
{
    (void)sender;
    if (_running) {
        [self requestStop];
        return;
    }
    if (!_pixels) {
        _status.stringValue = @"The video preview could not be initialized.";
        return;
    }

    os_unfair_lock_lock(&_settingsLock);
    _stopping = false;
    os_unfair_lock_unlock(&_settingsLock);
    _firstFrame = NO;
    _running = YES;
    _startButton.title = @"Stop Preview";
    _input.enabled = _firmwareButton.enabled = NO;
    _status.stringValue = @"Connecting to the USB-Live2…";
    [self startAudio];

    NSURL *firmware = _firmware;
    BOOL composite = _input.indexOfSelectedItem == 1;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            const struct capture_monitor monitor = {
                .context = (__bridge void *)self, .video = monitor_video,
                .audio = monitor_audio, .settings = monitor_settings,
                .recording = monitor_recording, .finish_recording = monitor_finish_recording,
            };
            const struct capture_options options = {
                .firmware_path = firmware.fileSystemRepresentation,
                .view_only = true, .svideo = !composite, .monitor = &monitor,
                .picture = {.initial = picture_defaults()},
            };
            int result = capture_run(&options);
            dispatch_async(dispatch_get_main_queue(), ^{ [self previewFinished:result]; });
        }
    });
}

- (void)requestStop
{
    os_unfair_lock_lock(&_settingsLock);
    _stopping = true;
    os_unfair_lock_unlock(&_settingsLock);
    _startButton.enabled = NO;
    _recordButton.enabled = NO;
    _status.stringValue = @"Stopping preview…";
}

- (void)previewFinished:(int)result
{
    // Capture has returned, so draining these queues cannot race a new sample from USB.
    dispatch_sync(_videoQueue, ^{});
    dispatch_sync(_audioQueue, ^{ [self->_player stop]; [self->_engine stop]; });
    [_renderer flushWithRemovalOfDisplayedImage:YES completionHandler:nil];
    _player = nil;
    _engine = nil;
    _running = NO;
    _previewReady = NO;
    _recordButton.enabled = NO;
    // An open recorder can be published while USB is shutting down, before it has been attached.
    bool stop;
    struct recorder *pending = [self takeRecorder:&stop];
    if (pending) {
        const struct capture_stats stats = {0};
        [self finishRecorder:pending stats:&stats reason:"preview_ended" complete:NO];
    }
    _status.stringValue = (_stopping || result == 0) ? @"Preview stopped." :
        @"Preview stopped. Check USB, firmware, input, and signal.";
    _input.enabled = _firmwareButton.enabled = YES;
    _startButton.enabled = _recordDirectory == nil;
    _startButton.title = @"Start Preview";
    if (_quitting && !_recordDirectory)
        [NSApp replyToApplicationShouldTerminate:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)application
{
    (void)application;
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)application
{
    (void)application;
    if (!_running && !_recordDirectory)
        return NSTerminateNow;
    // Let the capture worker restore the decoder and release the interfaces before exiting.
    _quitting = YES;
    if (_running)
        [self requestStop];
    return NSTerminateLater;
}

- (void)dealloc
{
    if (_pixels)
        CVPixelBufferPoolRelease(_pixels);
}

@end

int main(void)
{
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        // NSApplication borrows its delegate; keep it alive through the event loop.
        __attribute__((objc_precise_lifetime)) MonitorController *controller = [MonitorController new];
        app.delegate = controller;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
