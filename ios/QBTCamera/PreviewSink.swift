import AVFoundation
import SwiftUI

/// Shows the processed frames (effects, framing, mirror) on the phone: exactly what the PC gets.
///
/// The renderer is only ever touched from the camera's video queue: flushing it from the main
/// thread while the video queue enqueues can deadlock both (that froze the app when toggling the
/// black screen). The main thread only flips flags.
final class PreviewSink {
    let layer = AVSampleBufferDisplayLayer()

    private let lock = NSLock()
    private var enabled = true
    private var needsFlush = false
    private var format: CMVideoFormatDescription?

    init() {
        layer.videoGravity = .resizeAspect
        layer.backgroundColor = UIColor.black.cgColor
    }

    var isEnabled: Bool { lock.withLock { enabled } }

    /// Any thread. The renderer is reset by the video queue before the next frame it shows.
    func setEnabled(_ value: Bool) {
        lock.withLock {
            if value && !enabled { needsFlush = true }
            enabled = value
        }
    }

    /// Called on the camera's video queue only.
    func show(_ pixelBuffer: CVPixelBuffer, pts: CMTime) {
        let flush: Bool? = lock.withLock {
            guard enabled else { return nil }
            defer { needsFlush = false }
            return needsFlush
        }
        guard let flush else { return }

        let renderer = layer.sampleBufferRenderer
        if flush || renderer.status == .failed || renderer.requiresFlushToResumeDecoding { renderer.flush() }
        // Never queue frames the screen isn't taking: each one holds a camera buffer, and a stalled
        // queue would drain the camera's buffer pool and stop the whole stream.
        guard renderer.isReadyForMoreMediaData else { return }

        if format == nil || !CMVideoFormatDescriptionMatchesImageBuffer(format!, imageBuffer: pixelBuffer) {
            format = nil
            CMVideoFormatDescriptionCreateForImageBuffer(allocator: nil, imageBuffer: pixelBuffer, formatDescriptionOut: &format)
        }
        guard let format else { return }
        var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: pts, decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        guard CMSampleBufferCreateReadyWithImageBuffer(allocator: nil, imageBuffer: pixelBuffer, formatDescription: format,
                                                       sampleTiming: &timing, sampleBufferOut: &sample) == noErr,
              let sample else { return }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dictionary = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dictionary, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        renderer.enqueue(sample)
    }
}

struct ProcessedPreview: UIViewRepresentable {
    let sink: PreviewSink

    func makeUIView(context: Context) -> HostView {
        let view = HostView()
        view.backgroundColor = .black
        view.layer.addSublayer(sink.layer)
        return view
    }

    func updateUIView(_ uiView: HostView, context: Context) {}

    final class HostView: UIView {
        override func layoutSubviews() {
            super.layoutSubviews()
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            layer.sublayers?.forEach { $0.frame = bounds }
            CATransaction.commit()
        }
    }
}
