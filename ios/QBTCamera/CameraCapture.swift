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
final class CameraCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureAudioDataOutputSampleBufferDelegate {
    let session = AVCaptureSession()

    /// Called on the video queue for every captured frame.
    var onFrame: ((CVPixelBuffer, CMTime) -> Void)?
    var onError: ((Error) -> Void)?
    /// Called on the session queue after every (re)configuration, e.g. to send capabilities to the PC.
    var onConfigured: ((AVCaptureDevice, CameraLens) -> Void)?
    /// Microphone sample buffers (~20 ms each), called on the audio queue.
    var onAudio: ((CMSampleBuffer) -> Void)?

    private let sessionQueue = DispatchQueue(label: "qbt.camera.session")
    private let videoQueue = DispatchQueue(label: "qbt.camera.video", qos: .userInteractive)
    private let output = AVCaptureVideoDataOutput()
    private var input: AVCaptureDeviceInput?
    private let audioQueue = DispatchQueue(label: "qbt.camera.audio", qos: .userInteractive)
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
        NotificationCenter.default.addObserver(self, selector: #selector(sessionRuntimeError),
                                               name: AVCaptureSession.runtimeErrorNotification, object: session)
    }

    /// Applies camera/quality/orientation and starts the session; safe to call again to reconfigure.
    func start(lens: CameraLens, preset: StreamPreset, orientation: VideoOrientation) {
        sessionQueue.async { [self] in
            self.orientation = orientation
            do {
                try configure(lens: lens, preset: preset)
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
            session.beginConfiguration()
            defer { session.commitConfiguration() }
            if enabled {
                guard audioInput == nil,
                      let microphone = AVCaptureDevice.default(for: .audio),
                      let newInput = try? AVCaptureDeviceInput(device: microphone),
                      session.canAddInput(newInput) else { return }
                session.addInput(newInput)
                audioInput = newInput
                if !session.outputs.contains(audioOutput), session.canAddOutput(audioOutput) {
                    audioOutput.setSampleBufferDelegate(self, queue: audioQueue)
                    session.addOutput(audioOutput)
                }
            } else if let audioInput {
                session.removeInput(audioInput)
                self.audioInput = nil
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

    @objc private func sessionRuntimeError() {
        sessionQueue.asyncAfter(deadline: .now() + 0.5) { [self] in
            if !session.isRunning { session.startRunning() }
        }
    }

    private func configure(lens: CameraLens, preset: StreamPreset) throws {
        guard let device = lens.device else { throw CaptureError.cameraUnavailable }
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
        try applyFormat(preset, to: device)

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

    private func applyFormat(_ preset: StreamPreset, to device: AVCaptureDevice) throws {
        let (width, height) = preset.dimensions
        let candidates = device.formats.filter { format in
            let dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            return dimensions.width == width && dimensions.height == height
                && CMFormatDescriptionGetMediaSubType(format.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        }
        guard let format = candidates.first(where: { $0.supports(fps: preset.fps) }) ?? candidates.first else { return }
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
        guard let connection = output.connection(with: .video),
              connection.isVideoRotationAngleSupported(angle),
              connection.videoRotationAngle != angle else { return }
        connection.videoRotationAngle = angle
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
