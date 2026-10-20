import AVFoundation

/// Converts microphone sample buffers (whatever the hardware delivers) to 48 kHz mono 16-bit PCM,
/// the fixed format the PC expects.
final class AudioResampler {
    static let outputFormat = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 48_000, channels: 1, interleaved: true)!

    private var converter: AVAudioConverter?
    private var inputFormat: AVAudioFormat?

    /// Called on the audio capture queue.
    func convert(_ sampleBuffer: CMSampleBuffer) -> Data? {
        guard let description = CMSampleBufferGetFormatDescription(sampleBuffer),
              let streamDescription = CMAudioFormatDescriptionGetStreamBasicDescription(description),
              let format = AVAudioFormat(streamDescription: streamDescription) else { return nil }
        let frames = AVAudioFrameCount(CMSampleBufferGetNumSamples(sampleBuffer))
        guard frames > 0 else { return nil }

        if inputFormat != format {
            inputFormat = format
            converter = AVAudioConverter(from: format, to: Self.outputFormat)
        }
        guard let converter,
              let input = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: frames) else { return nil }
        input.frameLength = frames
        guard CMSampleBufferCopyPCMDataIntoAudioBufferList(sampleBuffer, at: 0, frameCount: Int32(frames),
                                                           into: input.mutableAudioBufferList) == noErr else { return nil }

        let capacity = AVAudioFrameCount(Double(frames) * Self.outputFormat.sampleRate / format.sampleRate) + 64
        guard let output = AVAudioPCMBuffer(pcmFormat: Self.outputFormat, frameCapacity: capacity) else { return nil }
        var supplied = false
        var error: NSError?
        let status = converter.convert(to: output, error: &error) { _, inputStatus in
            if supplied {
                inputStatus.pointee = .noDataNow
                return nil
            }
            supplied = true
            inputStatus.pointee = .haveData
            return input
        }
        guard status != .error, output.frameLength > 0, let samples = output.int16ChannelData else { return nil }
        return Data(bytes: samples[0], count: Int(output.frameLength) * MemoryLayout<Int16>.size)
    }
}
