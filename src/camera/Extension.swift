// SPDX-License-Identifier: GPL-2.0-or-later
import CoreMediaIO
import CoreVideo
import Foundation
import IOKit.audio
import OSLog
import Security

private let deviceID = UUID(uuidString: "CC5F6F94-05AF-4F40-988F-BA728A2F76A9")!
private let sourceID = UUID(uuidString: "BF281A26-D08E-422A-9B44-D5D06B56BDC1")!
private let sinkID = UUID(uuidString: "FA674E84-46C2-4E14-AC9D-E642742902A3")!
private let frameDuration = CMTime(value: 1001, timescale: 60000)
private let log = Logger(subsystem: "USB-Live2", category: "camera")

private enum CameraError: Error {
    case format(OSStatus), unsupportedStreamSetting, unauthorizedPublisher
}

/// Owns one cancellable worker. Delivery never blocks USB while the CMIO queue is busy.
private final class CameraCapture {
    private let lock = NSLock()
    private var stopping = false
    private let wake = DispatchSemaphore(value: 0)
    let frameSlots = DispatchSemaphore(value: 2)
    private let deliver: (CameraCapture, CMSampleBuffer) -> Void

    init(deliver: @escaping (CameraCapture, CMSampleBuffer) -> Void) {
        self.deliver = deliver
    }

    private var isStopped: Bool {
        lock.lock()
        defer { lock.unlock() }
        return stopping
    }

    func stop() {
        lock.lock()
        stopping = true
        lock.unlock()
        wake.signal()
    }

    func run(firmware: String) {
        let context = Unmanaged.passUnretained(self).toOpaque()
        var reportedError: String?
        while !isStopped {
            let reason = firmware.withCString { path in
                camera_capture(path, { context in
                    Unmanaged<CameraCapture>.fromOpaque(context).takeUnretainedValue().isStopped
                }, { context, sample in
                    let capture = Unmanaged<CameraCapture>.fromOpaque(context).takeUnretainedValue()
                    capture.deliver(capture, sample)
                }, context)
            }
            guard !isStopped, let reason else { return }
            let failure = String(cString: reason)
            if failure != reportedError {
                log.error("USB-Live2 capture: \(failure, privacy: .public)")
                reportedError = failure
            }
            // Keep the camera available when a user plugs in the device or restores the signal.
            _ = wake.wait(timeout: .now() + .seconds(1))
        }
    }
}

/// Both streams use this serial queue for lifecycle changes and sample delivery.
private final class Camera: NSObject, CMIOExtensionDeviceSource {
    let queue = DispatchQueue(label: "usblive2.camera")
    let format: CMIOExtensionStreamFormat
    let publisherRequirement: SecRequirement
    private let firmware: String
    private let captureQueue = DispatchQueue(label: "usblive2.camera.usb", qos: .userInitiated)
    private var capture: CameraCapture?
    private var sourceClients = 0
    private(set) var device: CMIOExtensionDevice!
    private var source: CameraStream!
    private var sink: CameraStream!
    private var timer: DispatchSourceTimer?
    private var publisher: CMIOExtensionClient?
    private var inFlight: UUID?

    init(publisherRequirement: SecRequirement, firmware: String) throws {
        self.firmware = firmware
        var description: CMVideoFormatDescription?
        let status = CMVideoFormatDescriptionCreate(
            allocator: kCFAllocatorDefault, codecType: kCVPixelFormatType_32BGRA,
            width: 640, height: 480, extensions: nil, formatDescriptionOut: &description)
        guard status == noErr, let description else { throw CameraError.format(status) }
        format = CMIOExtensionStreamFormat(formatDescription: description,
            maxFrameDuration: frameDuration, minFrameDuration: frameDuration,
            validFrameDurations: nil)
        self.publisherRequirement = publisherRequirement
        super.init()
        device = CMIOExtensionDevice(localizedName: "USB-Live2", deviceID: deviceID,
                                    legacyDeviceID: nil, source: self)
        source = CameraStream(camera: self, direction: .source, id: sourceID)
        sink = CameraStream(camera: self, direction: .sink, id: sinkID)
        try device.addStream(source.stream)
        try device.addStream(sink.stream)
    }

    var availableProperties: Set<CMIOExtensionProperty> { [.deviceTransportType, .deviceModel] }

    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionDeviceProperties {
        let result = CMIOExtensionDeviceProperties(dictionary: [:])
        if properties.contains(.deviceTransportType) {
            result.transportType = Int(kIOAudioDeviceTransportTypeUSB)
        }
        if properties.contains(.deviceModel) { result.model = "USB-Live2" }
        return result
    }

    func setDeviceProperties(_ properties: CMIOExtensionDeviceProperties) throws {}

    func startSource() {
        sourceClients += 1
        startCapture()
    }

    func stopSource() {
        sourceClients = max(0, sourceClients - 1)
        if sourceClients == 0 { stopCapture() }
    }

    private func startCapture() {
        guard capture == nil, timer == nil, sourceClients > 0 else { return }
        let session = CameraCapture { [weak self] session, sample in
            guard let self, session.frameSlots.wait(timeout: .now()) == .success else { return }
            self.queue.async {
                defer { session.frameSlots.signal() }
                // Discard samples queued before a stop or handoff to the optional publisher.
                guard self.capture === session else { return }
                self.send(sample, flags: [])
            }
        }
        capture = session
        captureQueue.async { [firmware] in session.run(firmware: firmware) }
    }

    private func stopCapture() {
        capture?.stop()
        capture = nil
    }

    private func send(_ sample: CMSampleBuffer, flags: CMIOExtensionStream.DiscontinuityFlags) {
        let pts = sample.presentationTimeStamp
        let ns = CMTimeConvertScale(pts, timescale: 1_000_000_000, method: .default).value
        guard pts.isNumeric, ns >= 0, !source.stream.streamingClients.isEmpty else { return }
        source.stream.send(sample, discontinuity: flags, hostTimeInNanoseconds: UInt64(ns))
    }

    /// The OS authorizes camera readers. Only our signed helper may write into the sink.
    func authorizePublisher(_ client: CMIOExtensionClient) -> Bool {
        guard client.pid > 0 else { return false }
        var guest: SecCode?
        let attributes = [kSecGuestAttributePid as String: NSNumber(value: client.pid)]
        guard SecCodeCopyGuestWithAttributes(nil, attributes as CFDictionary, [], &guest) == errSecSuccess,
              let guest,
              SecCodeCheckValidity(guest, [], publisherRequirement) == errSecSuccess else { return false }
        if let publisher, publisher.clientID != client.clientID { return false }
        publisher = client
        return true
    }

    func startSink() throws {
        guard publisher != nil else { throw CameraError.unauthorizedPublisher }
        guard timer == nil else { return }
        stopCapture()
        // The helper acknowledges readiness only after the direct USB worker releases its claims.
        captureQueue.sync {}
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(8), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.consume() }
        self.timer = timer
        timer.resume()
    }

    func stopSink() {
        timer?.cancel()
        timer = nil
        publisher = nil
        inFlight = nil
        startCapture()
    }

    private func consume() {
        guard inFlight == nil, let publisher else { return }
        let request = UUID()
        inFlight = request
        sink.stream.consumeSampleBuffer(from: publisher) { [weak self] sample, sequence, flags, more, _ in
            guard let self else { return }
            self.queue.async {
                // Ignore an old completion after a publisher disconnected and another started.
                guard self.inFlight == request else { return }
                self.inFlight = nil
                guard let sample else { return }
                let pts = sample.presentationTimeStamp
                let ns = CMTimeConvertScale(pts, timescale: 1_000_000_000, method: .default).value
                guard pts.isNumeric, ns >= 0 else { return }
                let hostTime = UInt64(ns)
                self.send(sample, flags: flags)
                self.sink.stream.notifyScheduledOutputChanged(
                    CMIOExtensionScheduledOutput(sequenceNumber: sequence, hostTimeInNanoseconds: hostTime))
                if more { self.consume() }
            }
        }
    }

    func disconnect(_ client: CMIOExtensionClient) {
        if publisher?.clientID == client.clientID { stopSink() }
    }
}

private final class CameraStream: NSObject, CMIOExtensionStreamSource {
    unowned let camera: Camera
    let direction: CMIOExtensionStream.Direction
    private(set) var stream: CMIOExtensionStream!

    init(camera: Camera, direction: CMIOExtensionStream.Direction, id: UUID) {
        self.camera = camera
        self.direction = direction
        super.init()
        let name = direction == .sink ? "USB-Live2 Publisher" : "USB-Live2 Video"
        stream = CMIOExtensionStream(localizedName: name, streamID: id, direction: direction,
                                     clockType: .hostTime, source: self)
    }

    var formats: [CMIOExtensionStreamFormat] { [camera.format] }
    var availableProperties: Set<CMIOExtensionProperty> {
        var properties: Set<CMIOExtensionProperty> = [.streamActiveFormatIndex, .streamFrameDuration]
        if direction == .sink {
            properties.formUnion([.streamSinkBufferQueueSize, .streamSinkBuffersRequiredForStartup])
        }
        return properties
    }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionStreamProperties {
        let result = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) { result.activeFormatIndex = 0 }
        if properties.contains(.streamFrameDuration) { result.frameDuration = frameDuration }
        if properties.contains(.streamSinkBufferQueueSize) { result.sinkBufferQueueSize = 4 }
        if properties.contains(.streamSinkBuffersRequiredForStartup) { result.sinkBuffersRequiredForStartup = 1 }
        return result
    }

    func setStreamProperties(_ properties: CMIOExtensionStreamProperties) throws {
        // A single format and frame duration are advertised for both ends of the relay.
        if let index = properties.activeFormatIndex, index != 0 {
            throw CameraError.unsupportedStreamSetting
        }
        if let duration = properties.frameDuration, CMTimeCompare(duration, frameDuration) != 0 {
            throw CameraError.unsupportedStreamSetting
        }
    }
    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool {
        direction == .source || camera.authorizePublisher(client)
    }
    func startStream() throws {
        if direction == .sink {
            try camera.startSink()
        } else {
            camera.startSource()
        }
    }
    func stopStream() throws {
        if direction == .sink {
            camera.stopSink()
        } else {
            camera.stopSource()
        }
    }
}

private final class Provider: NSObject, CMIOExtensionProviderSource {
    let camera: Camera
    private(set) var provider: CMIOExtensionProvider!

    init(camera: Camera) throws {
        self.camera = camera
        super.init()
        provider = CMIOExtensionProvider(source: self, clientQueue: camera.queue)
        try provider.addDevice(camera.device)
    }

    var availableProperties: Set<CMIOExtensionProperty> { [.providerName] }
    func connect(to client: CMIOExtensionClient) throws {}
    func disconnect(from client: CMIOExtensionClient) { camera.disconnect(client) }
    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionProviderProperties {
        let result = CMIOExtensionProviderProperties(dictionary: [:])
        if properties.contains(.providerName) { result.name = "USB-Live2" }
        return result
    }
    func setProviderProperties(_ properties: CMIOExtensionProviderProperties) throws {}
}

guard let identity = Bundle.main.object(forInfoDictionaryKey: "PublisherSigningIdentifier") as? String,
      let team = Bundle.main.object(forInfoDictionaryKey: "PublisherTeamIdentifier") as? String else {
    fatalError("The camera bundle is missing its publisher identity.")
}
var requirement: SecRequirement?
let expression = "anchor apple generic and identifier \"\(identity)\" and certificate leaf[subject.OU] = \"\(team)\""
guard SecRequirementCreateWithString(expression as CFString, [], &requirement) == errSecSuccess,
      let requirement else { fatalError("Invalid camera publisher identity.") }
guard let firmware = Bundle.main.path(forResource: "v4l-cx231xx-avcore-01", ofType: "fw",
                                      inDirectory: "firmware") else {
    fatalError("The camera bundle is missing its firmware.")
}
do {
    let provider = try Provider(camera: Camera(publisherRequirement: requirement, firmware: firmware))
    CMIOExtensionProvider.startService(provider: provider.provider)
    withExtendedLifetime(provider) { CFRunLoopRun() }
} catch {
    log.error("Unable to start USB-Live2: \(String(describing: error), privacy: .public)")
    exit(1)
}
