// SPDX-License-Identifier: GPL-2.0-or-later
#import "Installer.h"

@implementation CameraInstaller

- (void)activate:(id)sender
{
    (void)sender;
    NSString *identifier = [NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".extension"];
    OSSystemExtensionRequest *request = [OSSystemExtensionRequest activationRequestForExtension:identifier
                                                                                           queue:dispatch_get_main_queue()];
    request.delegate = self;
    [OSSystemExtensionManager.sharedManager submitRequest:request];
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    (void)request;
    [self showMessage:@"Approve USB-Live2 Camera in System Settings"
               detail:@"Open General → Login Items & Extensions and enable USB-Live2 Camera."
               style:NSAlertStyleInformational];
}

- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                   actionForReplacingExtension:(OSSystemExtensionProperties *)existing
                                  withExtension:(OSSystemExtensionProperties *)replacement
{
    (void)request;
    (void)existing;
    (void)replacement;
    return OSSystemExtensionReplacementActionReplace;
}

- (void)request:(OSSystemExtensionRequest *)request didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    (void)request;
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        [self showMessage:@"Restart to finish activation"
                   detail:@"USB-Live2 Camera will be available to other apps after restarting."
                   style:NSAlertStyleInformational];
    } else {
        [self showMessage:@"Camera extension is active"
                   detail:@"Other apps can now select USB-Live2 as a camera."
                   style:NSAlertStyleInformational];
    }
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
    (void)request;
    [self showMessage:@"Camera extension installation failed" detail:error.localizedDescription
               style:NSAlertStyleWarning];
}

- (void)showMessage:(NSString *)message detail:(NSString *)detail style:(NSAlertStyle)style
{
    NSAlert *alert = [NSAlert new];
    alert.messageText = message;
    alert.informativeText = detail;
    alert.alertStyle = style;
    [alert beginSheetModalForWindow:NSApp.mainWindow completionHandler:nil];
}

@end
