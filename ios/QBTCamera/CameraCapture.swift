import AVFoundation

enum CaptureError: LocalizedError {
    case cameraUnavailable
    case cannotAddInput
    case cannotAddOutput

    var errorDescription: String? {
        switch self {
        case .cameraUnavailable: "Камера недоступна."
        case .cannotAddInput, .cannotAddOutput: "Не удалось настроить камеру."
        }
    }
}

/// Owns the AVCaptureSession and hands raw NV12 frames to `onFrame` with no extra copies,
/// and (when enabled) microphone audio to `onAudio`.
final class CameraCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureAudioDataOutputSampleBufferDelegate,
    AVCaptureDepthDataOutputDelegate {
    let session = AVCaptureSession()

    /// Called on the video queue for every captured frame.
    var onFrame: ((CVPixelBuffer, CMTime) -> Void)?
    var onError: ((Error) -> Void)?
    /// Called on the session queue after every (re)configuration, e.g. to send capabilities to the PC.
    var onConfigured: ((AVCaptureDevice, CameraLens) -> Void)?
    /// Microphone sample buffers (~20 ms each), called on the audio queue.
    var onAudio: ((CMSampleBuffer) -> Void)?
    /// Depth maps (LiDAR / TrueDepth) while depth is on, nil when it turns off. Called on the depth queue.
    var onDepth: ((AVDepthData?) -> Void)?

    private let sessionQueue = DispatchQueue(label: "qbt.camera.session")
    private let videoQueue = DispatchQueue(label: "qbt.camera.video", qos: .userInteractive)
    private let output = AVCaptureVideoDataOutput()
    private var input: AVCaptureDeviceInput?
    private let depthOutput = AVCaptureDepthDataOutput()
    private let depthQueue = DispatchQueue(label: "qbt.camera.depth", qos: .userInteractive)
    private let audioQueue = DispatchQueue(label: "qbt.camera.audio", qos: .userInteractive)
    // The microphone has its own session: iOS stops the camera session when the app goes to the
    // background, while an audio-only session keeps running there (background audio mode).
    private let audioSession = AVCaptureSession()
    private let audioOutput = AVCaptureAudioDataOutput()
    private var audioInput: AVCaptureDeviceInput?
    private var lens = CameraLens.wide
    private var orientation = VideoOrientation.auto
    private var rotationCoordinator: AVCaptureDevice.RotationCoordinator?
    private var rotationObservation: NSKeyValueObservation?

    override init() {
        super.init()
        output.alwaysDiscardsLateVideoFrames = true
        // Native format of the sensor and the hardware encoder: no conversion anywhere.
        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
        output.setSampleBufferDelegate(self, queue: videoQueue)
        // Only the microphone session may own the app's audio session (it keeps running in the background).
        session.automaticallyConfiguresApplicationAudioSession = false
        NotificationCenter.default.addObserver(self, selector: #selector(sessionRuntimeError),
                                               name: AVCaptureSession.runtimeErrorNotification, object: session)
    }

    /// Applies camera/quality/orientation and starts the session; safe to call again to reconfigure.
    /// `depth`: use the LiDAR / TrueDepth version of the lens when the format allows it.
    func start(lens: CameraLens, preset: StreamPreset, orientation: VideoOrientation, depth: Bool = false) {
        sessionQueue.async { [self] in
            self.orientation = orientation
            do {
                try configure(lens: lens, preset: preset, depth: depth)
            } catch {
                onError?(error)
                return
            }
            if !session.isRunning { session.startRunning() }
            if let device = input?.device { onConfigured?(device, lens) }
        }
    }

    /// Adds or removes the microphone. Microphone permission must already be granted.
    func setAudioEnabled(_ enabled: Bool) {
        sessionQueue.async { [self] in
            if enabled {
                if audioInput == nil {
                    audioSession.beginConfiguration()
                    if let microphone = AVCaptureDevice.default(for: .audio),
                       let newInput = try? AVCaptureDeviceInput(device: microphone),
                       audioSession.canAddInput(newInput) {
                        audioSession.addInput(newInput)
                        audioInput = newInput
                    }
                    if !audioSession.outputs.contains(audioOutput), audioSession.canAddOutput(audioOutput) {
                        audioOutput.setSampleBufferDelegate(self, queue: audioQueue)
                        audioSession.addOutput(audioOutput)
                    }
                    audioSession.commitConfiguration()
                }
                if audioInput != nil, !audioSession.isRunning { audioSession.startRunning() }
            } else {
                if audioSession.isRunning { audioSession.stopRunning() }
                if let audioInput {
                    audioSession.beginConfiguration()
                    audioSession.removeInput(audioInput)
                    audioSession.commitConfiguration()
                    self.audioInput = nil
                }
            }
        }
    }

    /// Runs `body` on the session queue with the active device locked for configuration.
    func withDevice(_ body: @escaping (AVCaptureDevice) -> Void) {
        sessionQueue.async { [self] in
            guard let device = input?.device else { return }
            do {
                try device.lockForConfiguration()
            } catch {
                onError?(error)
                return
            }
            body(device)
            device.unlockForConfiguration()
        }
    }

    /// Runs `body` on the session queue with the active device (read-only access).
    func readDevice(_ body: @escaping (AVCaptureDevice, CameraLens) -> Void) {
        sessionQueue.async { [self] in
            guard let device = input?.device else { return }
            body(device, lens)
        }
    }

    func setOrientation(_ orientation: VideoOrientation) {
        sessionQueue.async { [self] in
            self.orientation = orientation
            applyRotation(orientation.fixedAngle ?? rotationCoordinator?.videoRotationAngleForHorizonLevelCapture ?? 0)
        }
    }

    func resume() {
        sessionQueue.async { [self] in
            if !session.isRunning && input != nil { session.startRunning() }
            if !audioSession.isRunning && audioInput != nil { audioSession.startRunning() }
        }
    }

    @objc private func sessionRuntimeError() {
        sessionQueue.asyncAfter(deadline: .now() + 0.5) { [self] in
            if !session.isRunning { session.startRunning() }
        }
    }

    private func configure(lens: CameraLens, preset: StreamPreset, depth: Bool) throws {
        // Depth devices only have depth in some formats (often not 4K): fall back to the plain lens.
        if depth, let depthDevice = lens.depthDevice, depthDevice.formats.contains(where: { Self.matches($0, preset, depth: true) }) {
            try configure(lens: lens, device: depthDevice, preset: preset, depth: true)
        } else {
            guard let device = lens.device else { throw CaptureError.cameraUnavailable }
            try configure(lens: lens, device: device, preset: preset, depth: false)
        }
    }

    private func configure(lens: CameraLens, device: AVCaptureDevice, preset: StreamPreset, depth: Bool) throws {
        self.lens = lens
        session.beginConfiguration()
        defer { session.commitConfiguration() }

        session.sessionPreset = .inputPriority // we pick activeFormat ourselves
        if let input {
            session.removeInput(input)
            self.input = nil
        }
        let newInput = try AVCaptureDeviceInput(device: device)
        guard session.canAddInput(newInput) else { throw CaptureError.cannotAddInput }
        session.addInput(newInput)
        input = newInput

        if !session.outputs.contains(output) {
            guard session.canAddOutput(output) else { throw CaptureError.cannotAddOutput }
            session.addOutput(output)
        }
        if depth {
            if !session.outputs.contains(depthOutput), session.canAddOutput(depthOutput) {
                session.addOutput(depthOutput)
                depthOutput.isFilteringEnabled = true // hole-filled, temporally smoothed depth
                depthOutput.alwaysDiscardsLateDepthData = true
                depthOutput.setDelegate(self, callbackQueue: depthQueue)
            }
        } else if session.outputs.contains(depthOutput) {
            session.removeOutput(depthOutput)
            depthQueue.async { [weak self] in self?.onDepth?(nil) }
        }
        try applyFormat(preset, to: device, depth: depth)

        if let connection = output.connection(with: .video) {
            // Stabilization buffers 1–3 frames: bad for latency.
            if connection.isVideoStabilizationSupported { connection.preferredVideoStabilizationMode = .off }
            // Call apps mirror the self-view themselves; mirroring here would flip text for everyone else.
            if connection.isVideoMirroringSupported {
                connection.automaticallyAdjustsVideoMirroring = false
                connection.isVideoMirrored = false
            }
        }
        trackRotation(of: device)
    }

    private static func matches(_ format: AVCaptureDevice.Format, _ preset: StreamPreset, depth: Bool) -> Bool {
        let dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
        let (width, height) = preset.dimensions
        let subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription)
        // Depth formats are often full-range only; effects convert everything to video range anyway.
        let pixelFormatOK = subtype == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            || (depth && subtype == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        // LiDAR only delivers depth with 4:3 formats: take the 4:3 one of the same width, the effects
        // crop it back to 16:9 on the GPU.
        let sizeOK = dimensions.width == width
            && (dimensions.height == height || (depth && dimensions.height == width * 3 / 4))
        return sizeOK && pixelFormatOK && (!depth || !format.supportedDepthDataFormats.isEmpty)
    }

    private func applyFormat(_ preset: StreamPreset, to device: AVCaptureDevice, depth: Bool) throws {
        let candidates = device.formats.filter { Self.matches($0, preset, depth: depth) }
        // Prefer the exact 16:9 size; a 4:3 depth format only when that's all there is.
        let (_, height) = preset.dimensions
        let ordered = candidates.sorted {
            (CMVideoFormatDescriptionGetDimensions($0.formatDescription).height == height ? 0 : 1)
                < (CMVideoFormatDescriptionGetDimensions($1.formatDescription).height == height ? 0 : 1)
        }
        guard let format = ordered.first(where: { $0.supports(fps: preset.fps) }) ?? ordered.first else { return }
        let fps = format.supports(fps: preset.fps) ? preset.fps : 30

        try device.lockForConfiguration()
        defer { device.unlockForConfiguration() }
        device.activeFormat = format
        if format.supports(fps: fps) {
            // Fixed frame rate: auto exposure must not stretch frames in low light.
            let duration = CMTime(value: 1, timescale: CMTimeScale(fps))
            device.activeVideoMinFrameDuration = duration
            device.activeVideoMaxFrameDuration = duration
        }
        if format.isVideoHDRSupported {
            device.automaticallyAdjustsVideoHDREnabled = false
            device.isVideoHDREnabled = false
        }
        if depth {
            // Largest float depth map the format offers (LiDAR ~320x240, TrueDepth ~640x480).
            let depthFormats = format.supportedDepthDataFormats.filter {
                let type = CMFormatDescriptionGetMediaSubType($0.formatDescription)
                return type == kCVPixelFormatType_DepthFloat16 || type == kCVPixelFormatType_DisparityFloat16
                    || type == kCVPixelFormatType_DepthFloat32 || type == kCVPixelFormatType_DisparityFloat32
            }
            device.activeDepthDataFormat = depthFormats.max {
                CMVideoFormatDescriptionGetDimensions($0.formatDescription).width
                    < CMVideoFormatDescriptionGetDimensions($1.formatDescription).width
            }
        }
    }

    private func trackRotation(of device: AVCaptureDevice) {
        rotationObservation?.invalidate()
        let coordinator = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: nil)
        rotationCoordinator = coordinator
        applyRotation(orientation.fixedAngle ?? coordinator.videoRotationAngleForHorizonLevelCapture)
        rotationObservation = coordinator.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) { [weak self] coordinator, _ in
            let angle = coordinator.videoRotationAngleForHorizonLevelCapture
            self?.sessionQueue.async {
                guard let self, self.orientation.fixedAngle == nil else { return }
                self.applyRotation(angle)
            }
        }
    }

    private func applyRotation(_ angle: CGFloat) {
        // The depth map must turn with the picture, or it wouldn't line up with the person.
        for connection in [output.connection(with: .video), depthOutput.connection(with: .depthData)].compactMap({ $0 }) {
            guard connection.isVideoRotationAngleSupported(angle), connection.videoRotationAngle != angle else { continue }
            connection.videoRotationAngle = angle
        }
    }

    func depthDataOutput(_ output: AVCaptureDepthDataOutput, didOutput depthData: AVDepthData, timestamp: CMTime,
                         connection: AVCaptureConnection) {
        onDepth?(depthData)
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        if output === audioOutput {
            onAudio?(sampleBuffer)
            return
        }
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        onFrame?(pixelBuffer, CMSampleBufferGetPresentationTimeStamp(sampleBuffer))
    }
}

private extension AVCaptureDevice.Format {
    func supports(fps: Int) -> Bool {
        videoSupportedFrameRateRanges.contains { $0.minFrameRate <= Double(fps) && Double(fps) <= $0.maxFrameRate }
    }
}
