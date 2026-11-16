// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Foundation

private enum IconError: Error {
    case failed(String)
}

/// Rasterize the vector afresh at each pixel size, retaining transparent corners.
private func png(_ image: NSImage, pixels: Int) throws -> Data {
    guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
        let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
        throw IconError.failed("Cannot allocate \(pixels)-pixel icon")
    }
    NSGraphicsContext.saveGraphicsState()
    defer { NSGraphicsContext.restoreGraphicsState() }
    NSGraphicsContext.current = context
    context.imageInterpolation = .high
    let bounds = NSRect(x: 0, y: 0, width: pixels, height: pixels)
    NSColor.clear.setFill()
    bounds.fill(using: .copy)
    image.draw(in: bounds, from: .zero, operation: .copy, fraction: 1)
    guard let data = bitmap.representation(using: .png, properties: [:]) else {
        throw IconError.failed("Cannot render \(pixels)-pixel icon")
    }
    // Catch failed/blank renders before shipping them into a signed bundle.
    guard bitmap.colorAt(x: pixels / 2, y: pixels / 2)?.alphaComponent == 1,
          bitmap.colorAt(x: 0, y: 0)?.alphaComponent == 0 else {
        throw IconError.failed("Icon must have an opaque center and transparent corners")
    }
    return data
}

/// Append an ICO integer without depending on host endianness or struct padding.
private func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
    var littleEndian = value.littleEndian
    withUnsafeBytes(of: &littleEndian) { data.append(contentsOf: $0) }
}

/// Build PNG, the complete macOS iconset/ICNS, and a PNG-backed Windows ICO (Vista or later).
private func build(source: URL, output: URL) throws {
    guard let image = NSImage(contentsOf: source), image.isValid else {
        throw IconError.failed("Cannot read SVG: \(source.path)")
    }
    let files = FileManager.default
    let iconset = output.appendingPathComponent("USB-Live2.iconset")
    try files.createDirectory(at: iconset, withIntermediateDirectories: true)
    var images: [Int: Data] = [:]
    for size in [16, 32, 48, 64, 128, 256, 512, 1024] {
        images[size] = try png(image, pixels: size)
    }
    for size in [16, 32, 128, 256, 512] {
        for scale in [1, 2] {
            let suffix = scale == 2 ? "@2x" : ""
            try images[size * scale]!.write(to: iconset.appendingPathComponent("icon_\(size)x\(size)\(suffix).png"), options: .atomic)
        }
    }
    try images[1024]!.write(to: output.appendingPathComponent("USB-Live2.png"), options: .atomic)
    let iconutil = Process()
    iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
    iconutil.arguments = ["-c", "icns", iconset.path, "-o", output.appendingPathComponent("USB-Live2.icns").path]
    try iconutil.run()
    iconutil.waitUntilExit()
    guard iconutil.terminationStatus == 0 else { throw IconError.failed("iconutil failed") }

    let sizes = [16, 32, 48, 64, 128, 256]
    var ico = Data()
    append(UInt16(0), to: &ico)
    append(UInt16(1), to: &ico)
    append(UInt16(sizes.count), to: &ico)
    var offset = 6 + 16 * sizes.count
    for size in sizes {
        let data = images[size]!
        // ICO encodes 256 as zero; each 16-byte directory entry points to an entire PNG.
        let dimension = UInt8(size % 256)
        ico.append(contentsOf: [dimension, dimension, 0, 0])
        append(UInt16(1), to: &ico)
        append(UInt16(32), to: &ico)
        append(UInt32(data.count), to: &ico)
        append(UInt32(offset), to: &ico)
        offset += data.count
    }
    for size in sizes { ico.append(images[size]!) }
    try ico.write(to: output.appendingPathComponent("USB-Live2.ico"), options: .atomic)
}

guard CommandLine.arguments.count == 3 else {
    fputs("Usage: build-icons SOURCE.svg OUTPUT_DIRECTORY\n", stderr)
    exit(2)
}
do {
    try build(source: URL(fileURLWithPath: CommandLine.arguments[1]),
              output: URL(fileURLWithPath: CommandLine.arguments[2]))
} catch {
    fputs("Icon generation failed: \(error)\n", stderr)
    exit(1)
}
