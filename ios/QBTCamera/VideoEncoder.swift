import VideoToolbox

/// Hardware H.264 encoder tuned for real-time: low-latency rate control, no B-frames, keyframes on demand.
final class VideoEncoder {
    /// Annex B access unit, keyframe flag, capture time in microseconds. Called on a VideoToolbox thread.
    var onEncodedFrame: ((Data, Bool, UInt64) -> Void)?

    private static let startCode: [UInt8] = [0, 0, 0, 1]

    private let lock = NSLock()
    private var session: VTCompressionSession?
    private var size = (width: Int32(0), height: Int32(0))
    private var fps = 30
    private var bitrate = 10_000_000
    private var bitrateChanged = false
    private var keyframeRequested = false
    private var needsReset = false

    deinit {
        if let session { VTCompressionSessionInvalidate(session) }
    }

    /// New settings take effect on the next frame (the session is rebuilt there, off the main thread:
    /// tearing down a busy 4K session can block for seconds and the system kills apps that hang).
    func configure(fps: Int, bitrate: Int) {
        lock.withLock {
            self.fps = fps
            self.bitrate = bitrate
            bitrateChanged = false
            needsReset = true
        }
    }

    /// Changes the target bitrate of the running session without a keyframe (adaptive bitrate).
    /// Applied on the encoding thread so the main thread never blocks on `VTSessionSetProperty`.
    func setBitrate(_ bitrate: Int) {
        lock.withLock {
            guard self.bitrate != bitrate else { return }
            self.bitrate = bitrate
            bitrateChanged = true
        }
    }

    func requestKeyframe() {
        lock.withLock { keyframeRequested = true }
    }

    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime) {
        let width = Int32(CVPixelBufferGetWidth(pixelBuffer))
        let height = Int32(CVPixelBufferGetHeight(pixelBuffer))
        let (session, forceKeyframe, newBitrate, retired): (VTCompressionSession?, Bool, Int?, VTCompressionSession?) = lock.withLock {
            // Rotation changes the buffer size: rebuild so the stream carries the new SPS.
            var retired: VTCompressionSession?
            if self.session == nil || needsReset || size.width != width || size.height != height {
                retired = takeSession()
                needsReset = false
                bitrateChanged = false
                self.session = makeSession(width: width, height: height)
                size = (width, height)
                keyframeRequested = true
            }
            let updatedBitrate = bitrateChanged ? bitrate : nil
            bitrateChanged = false
            defer { keyframeRequested = false }
            return (self.session, keyframeRequested, updatedBitrate, retired)
        }
        if let retired { VTCompressionSessionInvalidate(retired) } // outside the lock: may wait for frames in flight
        guard let session else { return }
        if let newBitrate {
            VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: newBitrate as CFNumber)
            VTSessionSetProperty(session, key: kVTCompressionPropertyKey_DataRateLimits,
                                 value: [newBitrate * 3 / 2 / 8, 1] as CFArray)
        }

        let properties = forceKeyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        let status = VTCompressionSessionEncodeFrame(session, imageBuffer: pixelBuffer, presentationTimeStamp: pts,
                                                     duration: .invalid, frameProperties: properties,
                                                     infoFlagsOut: nil) { [weak self] status, _, sampleBuffer in
            guard status == noErr, let sampleBuffer, let self else { return }
            self.deliver(sampleBuffer)
        }
        if status == kVTInvalidSessionErr { // happens after the app returns from background
            if let retired = lock.withLock({ takeSession() }) { VTCompressionSessionInvalidate(retired) }
        }
    }

    // Must be called with `lock` held. The caller invalidates the returned session after unlocking.
    private func takeSession() -> VTCompressionSession? {
        defer {
            session = nil
            size = (0, 0)
        }
        return session
    }

    // Must be called with `lock` held.
    private func makeSession(width: Int32, height: Int32) -> VTCompressionSession? {
        var session: VTCompressionSession?
        let lowLatency = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl: true] as CFDictionary
        var status = VTCompressionSessionCreate(allocator: nil, width: width, height: height,
                                                codecType: kCMVideoCodecType_H264, encoderSpecification: lowLatency,
                                                imageBufferAttributes: nil, compressedDataAllocator: nil,
                                                outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        if status != noErr {
            status = VTCompressionSessionCreate(allocator: nil, width: width, height: height,
                                                codecType: kCMVideoCodecType_H264, encoderSpecification: nil,
                                                imageBufferAttributes: nil, compressedDataAllocator: nil,
                                                outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        }
        guard status == noErr, let session else { return nil }

        func set(_ key: CFString, _ value: CFTypeRef) {
            VTSessionSetProperty(session, key: key, value: value)
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse) // no B-frames
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate as CFNumber)
        set(kVTCompressionPropertyKey_DataRateLimits, [bitrate * 3 / 2 / 8, 1] as CFArray) // bytes per second cap
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps as CFNumber)
        // Rare periodic keyframes; the receiver asks for one whenever it (re)connects.
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, (fps * 4) as CFNumber)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 4 as CFNumber)
        VTCompressionSessionPrepareToEncodeFrames(session)
        return session
    }

    /// AVCC (length-prefixed NAL units) → Annex B (start codes), SPS/PPS in front of keyframes.
    private func deliver(_ sampleBuffer: CMSampleBuffer) {
        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[CFString: Any]]
        let isKeyframe = (attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool) != true

        let length = CMBlockBufferGetDataLength(dataBuffer)
        var avcc = Data(count: length)
        let copyStatus = avcc.withUnsafeMutableBytes {
            CMBlockBufferCopyDataBytes(dataBuffer, atOffset: 0, dataLength: length, destination: $0.baseAddress!)
        }
        guard copyStatus == noErr else { return }

        var annexB = Data(capacity: length + 64)
        if isKeyframe, let format = CMSampleBufferGetFormatDescription(sampleBuffer) {
            appendParameterSets(of: format, to: &annexB)
        }
        avcc.withUnsafeBytes { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            var offset = 0
            while offset + 4 <= length {
                let nalLength = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16
                    | Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
                offset += 4
                guard nalLength > 0, offset + nalLength <= length else { break }
                annexB.append(contentsOf: Self.startCode)
                annexB.append(bytes.baseAddress! + offset, count: nalLength)
                offset += nalLength
            }
        }

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let ptsMicros = pts.isValid ? UInt64(max(0, pts.seconds) * 1_000_000) : 0
        onEncodedFrame?(annexB, isKeyframe, ptsMicros)
    }

    private func appendParameterSets(of format: CMFormatDescription, to data: inout Data) {
        var count = 0
        guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                                 parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                                 nalUnitHeaderLengthOut: nil) == noErr else { return }
        for index in 0..<count {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index,
                                                                     parameterSetPointerOut: &pointer,
                                                                     parameterSetSizeOut: &size, parameterSetCountOut: nil,
                                                                     nalUnitHeaderLengthOut: nil) == noErr,
                  let pointer else { continue }
            data.append(contentsOf: Self.startCode)
            data.append(pointer, count: size)
        }
    }
}
