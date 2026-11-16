#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
cd "$(dirname "$0")/../.."

team=${TEAM_ID:-UNSIGNED}
host_id=${CAMERA_BUNDLE_ID:-org.usblive2.camera}
extension_id="$host_id.extension"
publisher_id="$host_id.publisher"
app_group="$team.$extension_id"
version=${CAMERA_BUILD_VERSION:-3}
pkg_config=${PKG_CONFIG:-pkg-config}
case "$host_id" in ''|*[!A-Za-z0-9.-]*) echo "Invalid CAMERA_BUNDLE_ID" >&2; exit 2 ;; esac
case "$version" in ''|*[!0-9]*) echo "Invalid CAMERA_BUILD_VERSION" >&2; exit 2 ;; esac

if [ -n "${SIGN_IDENTITY:-}" ]; then
    case "$team" in
        *[!A-Z0-9]*|UNSIGNED) echo "Set TEAM_ID for your Apple signing identity." >&2; exit 2 ;;
    esac
    if [ "${#team}" -ne 10 ]; then
        echo "TEAM_ID must have 10 characters." >&2
        exit 2
    fi
    : "${HOST_PROFILE:?Set HOST_PROFILE to the host app provisioning profile}"
    : "${EXTENSION_PROFILE:?Set EXTENSION_PROFILE to the extension provisioning profile}"
    test -r "$HOST_PROFILE"
    test -r "$EXTENSION_PROFILE"
fi

root="$PWD/dist"
mkdir -p "$root"
work=$(mktemp -d "$root/.camera.XXXXXX")
trap 'rm -rf "$work"' EXIT
app="$work/USB-Live2 Camera.app"
host="$app/Contents"
extension="$host/Library/SystemExtensions/$extension_id.systemextension"
ext="$extension/Contents"
mkdir -p "$host/MacOS" "$host/Helpers" "$host/Frameworks" "$host/Resources/firmware" "$ext/MacOS" "$ext/Frameworks" \
    "$ext/Resources/firmware" "$work/objects"
# Installed older publishers use an input-only protocol. Advertise support for the readiness reply.
: > "$host/Resources/publisher-usb-handoff"

# Use the same diagnostics as the CLI, including when this script is run directly.
warnings=${WARNING_FLAGS:-$(make --no-print-directory print-warning-flags)}
libusb_cflags=$("$pkg_config" --cflags libusb-1.0 | sed 's/-I/-isystem /g')
libusb_dir=$("$pkg_config" --variable=libdir libusb-1.0)
libusb="$ext/Frameworks/libusb-1.0.dylib"
cp -L "$libusb_dir/libusb-1.0.dylib" "$libusb"
arch=$(uname -m)
build_info=$(xcrun vtool -arch "$arch" -show-build "$libusb")
libusb_min=$(printf '%s\n' "$build_info" | awk '
    $1 == "minos" { print $2; exit }
    $2 == "LC_VERSION_MIN_MACOSX" { legacy = 1 }
    legacy && $1 == "version" { print $2; exit }
')
minimum=13.0
# The embedded libusb can raise the deployment floor of the entire app.
if [ -n "$libusb_min" ] && [ "${libusb_min%%.*}" -ge 13 ]; then minimum=$libusb_min; fi
# The OS copies the extension out of the host app when activating it.
codesign --remove-signature "$libusb"
install_name_tool -id '@loader_path/../Frameworks/libusb-1.0.dylib' "$libusb"
cp firmware/v4l-cx231xx-avcore-01.fw firmware/LICENSE.conexant "$ext/Resources/firmware/"
cp firmware/v4l-cx231xx-avcore-01.fw firmware/LICENSE.conexant "$host/Resources/firmware/"
make icons
cp build/icons/USB-Live2.icns "$host/Resources/"

host_bin="$host/MacOS/USB-Live2 Camera"
ffmpeg_dir=$("$pkg_config" --variable=libdir libavformat)
make PKG_CONFIG="$pkg_config" CAPTURE_APP_EXECUTABLE="$host_bin" CAMERA_INSTALLER=1 capture-app-executable
host_min=$(python3 scripts/bundle-dylibs.py "$host_bin" "$host/Frameworks" "$libusb_dir" "$ffmpeg_dir")

cat > "$host/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>$host_id</string>
<key>CFBundleName</key><string>USB-Live2 Camera</string>
<key>CFBundleIconFile</key><string>USB-Live2.icns</string>
<key>CFBundleExecutable</key><string>USB-Live2 Camera</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>$version</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>LSMinimumSystemVersion</key><string>$host_min</string>
<key>NSCameraUsageDescription</key><string>Publishes USB-Live2 video to the installed camera extension.</string>
<key>NSSystemExtensionUsageDescription</key><string>Captures video from the USB-Live2 and publishes it as a macOS camera.</string>
</dict></plist>
PLIST
cat > "$ext/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>$extension_id</string>
<key>CFBundleName</key><string>USB-Live2 Camera</string>
<key>CFBundleDisplayName</key><string>USB-Live2 Camera</string>
<key>CFBundleExecutable</key><string>$extension_id</string>
<key>CFBundlePackageType</key><string>SYSX</string>
<key>CFBundleVersion</key><string>$version</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>LSMinimumSystemVersion</key><string>$minimum</string>
<key>NSSystemExtensionUsageDescription</key><string>Publishes USB-Live2 video as a macOS camera.</string>
<key>CMIOExtension</key><dict>
<key>CMIOExtensionMachServiceName</key><string>$app_group.camera</string>
</dict>
<key>PublisherSigningIdentifier</key><string>$publisher_id</string>
<key>PublisherTeamIdentifier</key><string>$team</string>
</dict></plist>
PLIST

for source in src/camera/Capture.c src/camera/Fields.c src/capture/cx231xx-init.c \
    src/capture/cx231xx-picture.c src/capture/cx231xx-usb.c src/capture/cx231xx-video.c; do
    object=$(basename "$source" .c).o
    xcrun clang -arch "$arch" -mmacosx-version-min="$minimum" -O2 -std=c11 $warnings -Werror \
        -Isrc/capture $libusb_cflags -c "$source" -o "$work/objects/$object"
done
xcrun swiftc -swift-version 5 -target "$arch-apple-macosx$minimum" -module-cache-path "$root/.swift-cache" \
    -O -warnings-as-errors -import-objc-header src/camera/Capture.h \
    src/camera/Extension.swift "$work"/objects/*.o "$libusb" -framework Accelerate -o "$ext/MacOS/$extension_id"
xcrun clang -mmacosx-version-min="$minimum" -O2 -std=c11 $warnings -Werror -Isrc/capture \
    src/camera/Publisher.c "$work/objects/Fields.o" -o "$host/Helpers/usblive2-camera-publish" \
    -framework CoreFoundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo -framework Accelerate
plutil -lint "$host/Info.plist" "$ext/Info.plist"
"$host/Helpers/usblive2-camera-publish" --check-fields

if [ -n "${SIGN_IDENTITY:-}" ]; then
    cp "$HOST_PROFILE" "$host/embedded.provisionprofile"
    cp "$EXTENSION_PROFILE" "$ext/embedded.provisionprofile"
    cat > "$work/host.entitlements" <<PLIST
<plist version="1.0"><dict>
<key>com.apple.developer.system-extension.install</key><true/>
<key>com.apple.application-identifier</key><string>$team.$host_id</string>
<key>com.apple.developer.team-identifier</key><string>$team</string>
<key>com.apple.security.application-groups</key><array><string>$app_group</string></array>
</dict></plist>
PLIST
    cat > "$work/extension.entitlements" <<PLIST
<plist version="1.0"><dict>
<key>com.apple.security.app-sandbox</key><true/>
<key>com.apple.security.device.usb</key><true/>
<key>com.apple.security.application-groups</key><array><string>$app_group</string></array>
<key>com.apple.application-identifier</key><string>$team.$extension_id</string>
<key>com.apple.developer.team-identifier</key><string>$team</string>
</dict></plist>
PLIST
    codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" --identifier "$publisher_id" \
        "$host/Helpers/usblive2-camera-publish"
    codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" "$libusb" "$host"/Frameworks/*.dylib
    codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" \
        --entitlements "$work/extension.entitlements" "$extension"
    codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" \
        --entitlements "$work/host.entitlements" "$app"
else
    codesign --force --sign - "$libusb" "$host"/Frameworks/*.dylib
    # Seal the rebuilt bundles for inspection without an Apple signing identity.
    codesign --force --sign - "$extension"
    codesign --force --sign - "$app"
    echo "Built for inspection. Provide Apple signing inputs to build an activatable app."
fi
codesign --verify --deep --strict "$app"

destination="$root/USB-Live2 Camera.app"
# Keep a previously signed app if an unsigned developer build is requested.
if [ -z "${SIGN_IDENTITY:-}" ]; then destination="$root/USB-Live2 Camera (unsigned).app"; fi
if [ -e "$destination" ]; then mv "$destination" "$work/previous.app"; fi
if ! mv "$app" "$destination"; then
    if [ -e "$work/previous.app" ]; then mv "$work/previous.app" "$destination"; fi
    exit 1
fi
echo "$destination"
