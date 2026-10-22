import AVFoundation
import Combine
import UIKit

/// Wires camera → effects → encoder → server, applies commands from the PC and the phone's own
/// controls, and publishes state for the UI.
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
    @Published private(set) var effectSettings = EffectSettings()
    @Published private(set) var cameraState = CameraUIState()
    @Published private(set) var thermal = ProcessInfo.ThermalState.nominal
    @Published private(set) var microphoneDenied = false
    /// App in the background: iOS stops the camera there, so the picture pauses while sound keeps going.
    @Published private(set) var paused = false
    /// Short hint that disappears by itself (e.g. why the torch did not turn on).
    @Published private(set) var notice: String?

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
    /// Send the iPhone microphone to the PC.
    @Published var audioEnabled: Bool {
        didSet {
            guard audioEnabled != oldValue else { return }
            defaults.set(audioEnabled, forKey: Keys.audio)
            applyAudio()
        }
    }

    /// Screen blacked out while streaming continues: the display is the biggest heat source on long calls.
    /// The on-phone preview is off meanwhile to save power.
    @Published var blackout = false {
        didSet {
            guard blackout != oldValue else { return }
            preview.setEnabled(!blackout)
        }
    }

    /// Bottom control panel unfolded (the dock always stays visible).
    @Published var panelExpanded = true
    @Published var activeTab: ControlTab = .background

    // Teleprompter near the front lens: read your script while looking into the camera.
    @Published var prompterVisible = false
    @Published var prompterPlaying = true
    @Published var prompterText: String {
        didSet { defaults.set(prompterText, forKey: Keys.prompterText) }
    }
    /// 1 (slow) … 5 (fast).
    @Published var prompterSpeed: Double {
        didSet { defaults.set(prompterSpeed, forKey: Keys.prompterSpeed) }
    }

    private enum Keys {
        static let lens = "lens"
        static let resolution = "resolution"
        static let fps = "fps"
        static let orientation = "orientation"
        static let effects = "effects"
        static let audio = "audio"
        static let prompterText = "prompterText"
        static let prompterSpeed = "prompterSpeed"
    }

    private let defaults = UserDefaults.standard

    let camera = CameraCapture()
    let preview = PreviewSink()
    let audioMeter = AudioMeter()
    private let effects = VideoEffects()
    private let resampler = AudioResampler()
    private let encoder = VideoEncoder()
    /// Matte side stream for PC-side background processing.
    private let auxEncoder = VideoEncoder()
    private let server = StreamServer(port: QBTProtocol.videoPort)
    private var statsTimer: Timer?
    private var stateTimer: Timer?
    private var started = false
    private var secondsWithoutDrops = 0
    private var noticeTask: Task<Void, Never>?

    /// Where the portrait effect gets its depth from, for the Background panel.
    var depthLabel: String {
        guard effects.hasDepth else {
            return lens.depthDevice == nil ? "silhouette only (this camera has no depth sensor)" : "silhouette only"
        }
        return lens == .front ? "Depth: TrueDepth" : "Depth: LiDAR"
    }

    /// "USB", "Wi-Fi" or a waiting hint, for the status island.
    var connectionLabel: String {
        guard let clientName else { return "Waiting for PC" }
        return clientName == "USB" ? "USB" : "Wi-Fi"
    }

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
        audioEnabled = defaults.object(forKey: Keys.audio) as? Bool ?? true
        prompterText = defaults.string(forKey: Keys.prompterText)
            ?? "Hi! This is the QBT Camera teleprompter. The text scrolls right next to the lens — read it while looking straight into the camera. Tap the pencil to edit it."
        let savedSpeed = defaults.double(forKey: Keys.prompterSpeed)
        prompterSpeed = savedSpeed > 0 ? savedSpeed : 2
        if let saved = defaults.dictionary(forKey: Keys.effects) {
            effectSettings = Self.effectSettings(from: saved, base: EffectSettings())
            effects.update(effectSettings)
        }

        Self.connectPipeline(camera: camera, effects: effects, preview: preview, encoder: encoder, auxEncoder: auxEncoder,
                             server: server)
        Self.connectAudio(camera: camera, resampler: resampler, meter: audioMeter, server: server)
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
    private nonisolated static func connectPipeline(camera: CameraCapture, effects: VideoEffects, preview: PreviewSink,
                                                   encoder: VideoEncoder, auxEncoder: VideoEncoder, server: StreamServer) {
        effects.onAuxFrame = { buffer, pts in
            if server.hasClient { auxEncoder.encode(buffer, pts: pts) }
        }
        auxEncoder.onEncodedFrame = { data, keyframe, ptsMicros in
            server.sendAux(frame: data, keyframe: keyframe, ptsMicros: ptsMicros)
        }
        camera.onFrame = { pixelBuffer, pts in
            let streaming = server.hasClient
            let previewing = preview.isEnabled
            guard streaming || previewing else { return } // screen off and nobody watching: do nothing
            // Every frame reaches the preview and the encoder through the effects' delivery queue,
            // in order (the preview must only be touched from one queue).
            effects.process(pixelBuffer, pts: pts) { output, pts in
                if previewing { preview.show(output, pts: pts) }
                if streaming { encoder.encode(output, pts: pts) }
            }
        }
        encoder.onEncodedFrame = { data, keyframe, ptsMicros in
            server.send(frame: data, keyframe: keyframe, ptsMicros: ptsMicros)
        }
        server.onKeyframeNeeded = {
            encoder.requestKeyframe()
            auxEncoder.requestKeyframe()
        }
        // New lens or format: tell the PC what the camera can do now.
        camera.onDepth = { depth in effects.updateDepth(depth) }
        camera.onFaces = { faces in effects.updateFaces(faces) }
        camera.onConfigured = { device, lens in
            if let json = CameraControl.encode(capabilities(of: device, lens: lens)) {
                server.sendMessage(json)
            }
        }
    }

    private nonisolated static func connectAudio(camera: CameraCapture, resampler: AudioResampler, meter: AudioMeter,
                                                server: StreamServer) {
        camera.onAudio = { sampleBuffer in
            guard let pcm = resampler.convert(sampleBuffer) else { return }
            meter.record(pcm)
            guard server.hasClient else { return }
            let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
            server.sendAudio(pcm, ptsMicros: pts.isValid ? UInt64(max(0, pts.seconds) * 1_000_000) : 0)
        }
    }

    private nonisolated static func capabilities(of device: AVCaptureDevice, lens: CameraLens) -> [String: Any] {
        var caps = CameraControl.capabilities(of: device, lens: lens)
        caps["filters"] = ColorFilter.allCases.map(\.rawValue).joined(separator: ",")
        caps["microphone"] = AVCaptureDevice.default(for: .audio) != nil
        caps["teleTitle"] = CameraLens.telephoto.title
        // Which depth sensor feeds the portrait effect right now.
        caps["depth"] = device.deviceType == .builtInLiDARDepthCamera ? "lidar"
            : device.deviceType == .builtInTrueDepthCamera ? "truedepth" : ""
        // Lets the PC fall back to Wi-Fi instantly when the cable is pulled, without scanning the network.
        caps["wifi"] = NetworkInfo.wifiAddress() ?? ""
        return caps
    }

    private nonisolated static func effectSettings(from values: [String: Any], base: EffectSettings) -> EffectSettings {
        var settings = base
        if let blur = values["blur"] as? NSNumber { settings.blur = blur.doubleValue.clamped(0, 1) }
        if let mode = (values["background"] as? String).flatMap(BackgroundMode.init(rawValue:)) {
            settings.background = mode
        } else if let blur = values["blur"] as? NSNumber { // older PC apps only know a blur slider
            if blur.doubleValue > 0.01, settings.background == .off { settings.background = .portrait }
            if blur.doubleValue <= 0.01, settings.background == .portrait { settings.background = .off }
        }
        if let backdrop = (values["backdrop"] as? String).flatMap(Backdrop.init(rawValue:)) { settings.backdrop = backdrop }
        if let value = values["keySensitivity"] as? NSNumber { settings.keySensitivity = value.doubleValue.clamped(0, 1) }
        if let value = values["keySoftness"] as? NSNumber { settings.keySoftness = value.doubleValue.clamped(0, 1) }
        if let value = values["keySpill"] as? NSNumber { settings.keySpill = value.doubleValue.clamped(0, 1) }
        if let processing = values["processing"] as? String { settings.processOnPC = processing != "phone" }
        if let retouch = values["retouch"] as? NSNumber { settings.retouch = retouch.doubleValue.clamped(0, 1) }
        if let framing = values["framing"] as? Bool { settings.framing = framing }
        if let filter = (values["filter"] as? String).flatMap(ColorFilter.init(rawValue:)) { settings.filter = filter }
        if let amount = values["filterAmount"] as? NSNumber { settings.filterAmount = amount.doubleValue.clamped(0, 1) }
        if let mirror = values["mirror"] as? Bool { settings.mirror = mirror }
        return settings
    }

    private nonisolated static func dictionary(of settings: EffectSettings) -> [String: Any] {
        ["background": settings.background.rawValue, "blur": settings.blur, "backdrop": settings.backdrop.rawValue,
         "keySensitivity": settings.keySensitivity, "keySoftness": settings.keySoftness, "keySpill": settings.keySpill,
         "processing": settings.processOnPC ? "pc" : "phone",
         "retouch": settings.retouch, "framing": settings.framing,
         "filter": settings.filter.rawValue, "filterAmount": settings.filterAmount, "mirror": settings.mirror]
    }

    func start() {
        guard !started else { return }
        started = true
        LiveActivityCleanup.endLeftovers()
        wifiAddress = NetworkInfo.wifiAddress()
        audioMeter.start()
        AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
            Task { @MainActor in
                guard let self else { return }
                guard granted else {
                    self.errorMessage = "No camera access. Allow it in Settings → QBT Camera → Camera."
                    return
                }
                self.applyCameraSettings()
                self.applyAudio()
                self.server.start()
            }
        }
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshStats() }
        }
        // Live values (ISO, shutter, focus…) for the sliders here and on the PC.
        stateTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshState() }
        }
    }

    /// Back in the foreground: iOS may have stopped the camera session or the listener meanwhile.
    func resume() {
        guard started else { return }
        paused = false
        server.start()
        camera.resume()
        refreshState()
    }

    /// The app went to the background: everything stops — camera, microphone, the connection to the PC.
    /// No background modes, no Live Activity: a minimized QBT Camera is fully off.
    func pause() {
        guard started else { return }
        paused = true
        refreshState() // the PC hears "paused" before the link drops
        server.stop()
        camera.stop()
    }

    func showNotice(_ text: String) {
        notice = text
        noticeTask?.cancel()
        noticeTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(3))
            guard !Task.isCancelled else { return }
            self?.notice = nil
        }
    }

    /// One command, from the PC (JSON) or from the phone's own controls — same format.
    func perform(_ command: [String: Any]) {
        guard let name = command["cmd"] as? String else { return }
        switch name {
        case "lens":
            if let value = command["value"] as? String, let lens = CameraLens(rawValue: value),
               CameraLens.available.contains(lens) {
                self.lens = lens // reconfigures; capabilities follow via onConfigured
            }
        case "audio":
            if let enabled = command["enabled"] as? Bool { audioEnabled = enabled }
        case "screen":
            if let value = command["blackout"] as? Bool { blackout = value }
            if let value = command["panel"] as? Bool { panelExpanded = value }
        case "tab":
            if let value = command["value"] as? String, let t = ControlTab(rawValue: value) {
                activeTab = t
                panelExpanded = true
            }
        case "orientation":
            if let value = (command["value"] as? String).flatMap(VideoOrientation.init(rawValue:)) { orientation = value }
        case "preset":
            var updated = preset
            if let resolution = (command["resolution"] as? String).flatMap(StreamResolution.init(rawValue:)) {
                updated.resolution = resolution
            }
            if let fps = (command["fps"] as? NSNumber)?.intValue, StreamPreset.frameRates.contains(fps) { updated.fps = fps }
            preset = updated
        case "effects":
            updateEffects(Self.effectSettings(from: command, base: effectSettings))
            if command["keyResample"] as? Bool == true { effects.resampleKeyColor() }
        case "prompter":
            if let text = command["text"] as? String { prompterText = text }
            if let visible = command["visible"] as? Bool { prompterVisible = visible }
            if let playing = command["playing"] as? Bool { prompterPlaying = playing }
            if let speed = command["speed"] as? NSNumber { prompterSpeed = speed.doubleValue.clamped(1, 5) }
        default:
            camera.withDevice { device in CameraControl.apply(command, to: device) }
        }
        refreshState()
    }

    func updateEffects(_ settings: EffectSettings) {
        let depthChanged = settings.wantsDepth != effectSettings.wantsDepth
        if settings.background == .chroma, effectSettings.background != .chroma { effects.resampleKeyColor() }
        effectSettings = settings
        effects.update(settings)
        defaults.set(Self.dictionary(of: settings), forKey: Keys.effects)
        if depthChanged { applyCameraSettings() } // switch to / from the LiDAR or TrueDepth camera
    }

    /// Chroma key: look at the screen color again (after the light changed, or a new backdrop).
    func resampleKeyColor() {
        effects.resampleKeyColor()
    }

    private func applyCameraSettings() {
        guard started else { return }
        errorMessage = nil
        targetBitrate = preset.bitrate
        secondsWithoutDrops = 0
        encoder.configure(fps: preset.fps, bitrate: preset.bitrate)
        auxEncoder.configure(fps: preset.fps, bitrate: 1_500_000)
        server.setFrameRate(preset.fps)
        camera.start(lens: lens, preset: preset, orientation: orientation, depth: effectSettings.wantsDepth)
    }

    private func applyAudio() {
        guard started else { return }
        guard audioEnabled else {
            camera.setAudioEnabled(false)
            return
        }
        AVCaptureDevice.requestAccess(for: .audio) { [weak self] granted in
            Task { @MainActor in
                guard let self, self.audioEnabled else { return }
                self.microphoneDenied = !granted
                self.camera.setAudioEnabled(granted)
                self.refreshState()
            }
        }
    }

    private func handleCommand(_ data: Data) {
        guard let command = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return }
        perform(command)
    }

    private func sendCapabilities() {
        let server = server
        camera.readDevice { device, lens in
            if let json = CameraControl.encode(Self.capabilities(of: device, lens: lens)) {
                server.sendMessage(json)
            }
        }
    }

    /// Reads the camera once: updates the phone's controls and, when connected, the PC's.
    private func refreshState() {
        thermal = ProcessInfo.processInfo.thermalState
        let server = server
        let effects = effects
        let connected = clientName != nil
        var extras: [String: Any] = [
            "audioEnabled": audioEnabled && !microphoneDenied,
            "microphoneDenied": microphoneDenied,
            "prompterVisible": prompterVisible,
            "prompterPlaying": prompterPlaying,
            "prompterSpeed": prompterSpeed,
            "blackout": blackout,
            "paused": paused,
            "depthActive": effects.hasDepth,
            "resolution": preset.resolution.rawValue,
            "fps": preset.fps,
            "orientation": orientation.rawValue,
        ]
        if let roundTripMillis { extras["rttMs"] = roundTripMillis }
        extras.merge(Self.dictionary(of: effects.current)) { _, effect in effect }
        camera.readDevice { [weak self] device, lens in
            var state = CameraControl.state(of: device, lens: lens)
            let ui = CameraUIState(state: state, caps: CameraControl.capabilities(of: device, lens: lens))
            Task { @MainActor in
                if self?.cameraState != ui { self?.cameraState = ui }
            }
            guard connected else { return }
            state.merge(extras) { _, extra in extra }
            state["effectsMs"] = effects.current.isActive ? effects.lastProcessingMillis : 0
            state["visionMs"] = effects.current.isActive ? effects.lastVisionMillis : 0
            if let json = CameraControl.encode(state) {
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
        if clientName != nil && !paused { adaptBitrate(droppedFrames: stats.dropped) }
        if !paused { camera.ensureRunning() } // recovers from thermal shutdowns and stuck sessions
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
