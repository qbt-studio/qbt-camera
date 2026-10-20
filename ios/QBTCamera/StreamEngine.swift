import AVFoundation
import Combine

/// Wires camera → encoder → server, applies commands from the PC and publishes state for the UI.
@MainActor
final class StreamEngine: ObservableObject {
    @Published private(set) var clientName: String?
    @Published private(set) var fps = 0
    @Published private(set) var megabitsPerSecond = 0.0
    @Published private(set) var droppedFrames = 0
    /// Send → acknowledgement time; nil until the PC acks frames.
    @Published private(set) var roundTripMillis: Double?
    /// Bitrate the encoder targets right now (lowered automatically when the link can't keep up).
    @Published private(set) var targetBitrate = 0
    @Published private(set) var errorMessage: String?
    @Published private(set) var wifiAddress: String?

    @Published var lens: CameraLens {
        didSet {
            guard lens != oldValue else { return }
            defaults.set(lens.rawValue, forKey: Keys.lens)
            applyCameraSettings()
        }
    }
    @Published var preset: StreamPreset {
        didSet {
            guard preset != oldValue else { return }
            defaults.set(preset.resolution.rawValue, forKey: Keys.resolution)
            defaults.set(preset.fps, forKey: Keys.fps)
            applyCameraSettings()
        }
    }
    @Published var orientation: VideoOrientation {
        didSet {
            defaults.set(orientation.rawValue, forKey: Keys.orientation)
            camera.setOrientation(orientation)
        }
    }

    private enum Keys {
        static let lens = "lens"
        static let resolution = "resolution"
        static let fps = "fps"
        static let orientation = "orientation"
    }

    private let defaults = UserDefaults.standard

    let camera = CameraCapture()
    private let encoder = VideoEncoder()
    private let server = StreamServer(port: QBTProtocol.videoPort)
    private var statsTimer: Timer?
    private var stateTimer: Timer?
    private var started = false
    private var secondsWithoutDrops = 0

    init() {
        // Restore the last used settings.
        let defaults = UserDefaults.standard
        let savedLens = defaults.string(forKey: Keys.lens).flatMap(CameraLens.init(rawValue:))
        lens = savedLens.flatMap { CameraLens.available.contains($0) ? $0 : nil } ?? .wide
        let savedFPS = defaults.integer(forKey: Keys.fps)
        preset = StreamPreset(
            resolution: defaults.string(forKey: Keys.resolution).flatMap(StreamResolution.init(rawValue:)) ?? .fullHD,
            fps: StreamPreset.frameRates.contains(savedFPS) ? savedFPS : 30)
        orientation = defaults.string(forKey: Keys.orientation).flatMap(VideoOrientation.init(rawValue:)) ?? .auto

        Self.connectPipeline(camera: camera, encoder: encoder, server: server)
        server.onClientChanged = { [weak self] name in
            Task { @MainActor in
                self?.clientName = name
                if name != nil { self?.sendCapabilities() }
            }
        }
        server.onCommand = { [weak self] data in
            Task { @MainActor in self?.handleCommand(data) }
        }
        camera.onError = { [weak self] error in
            Task { @MainActor in self?.errorMessage = error.localizedDescription }
        }
    }

    /// Nonisolated so these per-frame closures are not tied to the main actor.
    private nonisolated static func connectPipeline(camera: CameraCapture, encoder: VideoEncoder, server: StreamServer) {
        camera.onFrame = { pixelBuffer, pts in
            guard server.hasClient else { return } // don't burn battery encoding for nobody
            encoder.encode(pixelBuffer, pts: pts)
        }
        encoder.onEncodedFrame = { data, keyframe, ptsMicros in
            server.send(frame: data, keyframe: keyframe, ptsMicros: ptsMicros)
        }
        server.onKeyframeNeeded = { encoder.requestKeyframe() }
        // New lens or format: tell the PC what the camera can do now.
        camera.onConfigured = { device, lens in
            if let json = CameraControl.encode(CameraControl.capabilities(of: device, lens: lens)) {
                server.sendMessage(json)
            }
        }
    }

    func start() {
        guard !started else { return }
        started = true
        wifiAddress = NetworkInfo.wifiAddress()
        AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
            Task { @MainActor in
                guard let self else { return }
                guard granted else {
                    self.errorMessage = "Нет доступа к камере. Разрешите его: Настройки → QBT Camera → Камера."
                    return
                }
                self.applyCameraSettings()
                self.server.start()
            }
        }
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshStats() }
        }
        // Live values (ISO, shutter, focus…) for the PC's sliders while auto modes adjust them.
        stateTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sendState() }
        }
    }

    private func applyCameraSettings() {
        guard started else { return }
        errorMessage = nil
        targetBitrate = preset.bitrate
        secondsWithoutDrops = 0
        encoder.configure(fps: preset.fps, bitrate: preset.bitrate)
        server.setFrameRate(preset.fps)
        camera.start(lens: lens, preset: preset, orientation: orientation)
    }

    private func handleCommand(_ data: Data) {
        guard let command = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let name = command["cmd"] as? String else { return }
        if name == "lens" {
            if let value = command["value"] as? String, let lens = CameraLens(rawValue: value),
               CameraLens.available.contains(lens) {
                self.lens = lens // reconfigures; capabilities follow via onConfigured
            }
            return
        }
        camera.withDevice { device in CameraControl.apply(command, to: device) }
        sendState()
    }

    private func sendCapabilities() {
        let server = server
        camera.readDevice { device, lens in
            if let json = CameraControl.encode(CameraControl.capabilities(of: device, lens: lens)) {
                server.sendMessage(json)
            }
        }
    }

    private func sendState() {
        guard clientName != nil else { return }
        let server = server
        camera.readDevice { device, lens in
            if let json = CameraControl.encode(CameraControl.state(of: device, lens: lens)) {
                server.sendMessage(json)
            }
        }
    }

    private func refreshStats() {
        let stats = server.takeStats()
        fps = stats.frames
        megabitsPerSecond = Double(stats.bytes) * 8 / 1_000_000
        droppedFrames += stats.dropped
        roundTripMillis = stats.roundTripMillis
        wifiAddress = NetworkInfo.wifiAddress()
        if clientName != nil { adaptBitrate(droppedFrames: stats.dropped) }
    }

    /// Back off fast when the link drops frames, recover slowly when it keeps up.
    private func adaptBitrate(droppedFrames: Int) {
        let floor = preset.bitrate * 3 / 10
        if droppedFrames > 0 {
            secondsWithoutDrops = 0
            let lowered = max(floor, targetBitrate * 3 / 4)
            guard lowered != targetBitrate else { return }
            targetBitrate = lowered
            encoder.setBitrate(lowered)
        } else if targetBitrate < preset.bitrate {
            secondsWithoutDrops += 1
            guard secondsWithoutDrops >= 3 else { return }
            secondsWithoutDrops = 0
            targetBitrate = min(preset.bitrate, targetBitrate * 11 / 10)
            encoder.setBitrate(targetBitrate)
        }
    }
}
