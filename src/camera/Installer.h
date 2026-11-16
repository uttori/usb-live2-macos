// SPDX-License-Identifier: GPL-2.0-or-later
#import <AppKit/AppKit.h>
#import <SystemExtensions/SystemExtensions.h>

/** Handles an explicit request to install the system extension without interrupting the capture app. */
@interface CameraInstaller : NSObject <OSSystemExtensionRequestDelegate>
- (void)activate:(id)sender;
@end
