import CoreImage
import CoreImage.CIFilterBuiltins
import Metal
import QuartzCore
import Vision

enum ColorFilter: String, CaseIterable, Identifiable {
    case none, warm, cool, cinema, vivid, soft, mono, noir

    var id: String { rawValue }
}

struct EffectSettings: Equatable {
    /// Background blur strength, 0 = off.
    var blur = 0.0
    /// Skin smoothing strength, 0 = off.
    var retouch = 0.0
    /// Keep the face(s) framed by cropping in, like Center Stage.
    var framing = false
    var filter = ColorFilter.none
    var filterAmount = 1.0
    /// Flip left-right before sending.
    var mirror = false

    var isActive: Bool { blur > 0.01 || retouch > 0.01 || framing || filter != .none || mirror }
}

/// Real-time effects applied on the phone before encoding: Vision finds people and faces on the
/// Neural Engine, Core Image renders on the GPU straight into an NV12 buffer for the encoder.
/// With every effect off, frames pass through untouched at zero cost.
///
/// Vision runs on its own queue and never blocks the camera: each frame is rendered with the
/// newest finished person mask and face list (at most a frame or two old, invisible in practice).
final class VideoEffects {
    /// Most a face-tracking crop zooms in; beyond this a 1080p picture gets visibly soft.
    private static let maxFramingZoom: CGFloat = 2.2

    private let lock = NSLock()
    private var settings = EffectSettings()
    // Results of the background Vision pass, guarded by `lock`.
    private var latestMask: CVPixelBuffer?
    private var latestFaces: [CGRect] = [] // normalized, bottom-left origin (Vision and Core Image agree)
    private var lastFaceTime = 0.0
    private var visionBusy = false
    private let visionQueue = DispatchQueue(label: "qbt.effects.vision", qos: .userInitiated)

    private let context: CIContext
    private let outputColorSpace = CGColorSpace(name: CGColorSpace.itur_709)!
    private let sequenceHandler = VNSequenceRequestHandler() // temporal smoothing across frames
    private let segmentation: VNGeneratePersonSegmentationRequest = {
        let request = VNGeneratePersonSegmentationRequest()
        request.qualityLevel = .balanced
        request.outputPixelFormat = kCVPixelFormatType_OneComponent8
        return request
    }()
    private let faceDetection = VNDetectFaceRectanglesRequest()

    private var pool: CVPixelBufferPool?
    private var poolSize = CGSize.zero
    private var faces: [CGRect] = [] // snapshot of latestFaces for the frame being rendered
    private var framingRect: CGRect?
    private var processingMillis = 0.0

    init() {
        if let device = MTLCreateSystemDefaultDevice() {
            context = CIContext(mtlDevice: device, options: [.cacheIntermediates: false])
        } else {
            context = CIContext(options: [.cacheIntermediates: false])
        }
    }

    func update(_ newSettings: EffectSettings) {
        lock.withLock { settings = newSettings }
    }

    var current: EffectSettings { lock.withLock { settings } }

    /// Average GPU+Vision time per processed frame, for the PC's status line.
    var lastProcessingMillis: Double { lock.withLock { processingMillis } }

    /// Called on the capture queue for every frame. Returns the input when no effect is on.
    func process(_ pixelBuffer: CVPixelBuffer) -> CVPixelBuffer {
        let settings = current
        guard settings.isActive else {
            framingRect = nil
            return pixelBuffer
        }
        let started = CACurrentMediaTime()
        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        let extent = CGRect(x: 0, y: 0, width: width, height: height)

        // 1. Vision in the background; use whatever it finished last.
        let wantsMask = settings.blur > 0.01
        let wantsFaces = settings.framing || settings.retouch > 0.01
        scheduleVision(on: pixelBuffer, mask: wantsMask, faces: wantsFaces)
        let mask: CVPixelBuffer?
        let lastFaceTime: Double
        (mask, faces, lastFaceTime) = lock.withLock {
            (wantsMask ? latestMask : nil, wantsFaces ? latestFaces : [], self.lastFaceTime)
        }

        // 2. Core Image chain: retouch → background blur → color → framing crop.
        var image = CIImage(cvPixelBuffer: pixelBuffer)
        if settings.retouch > 0.01, !faces.isEmpty {
            image = retouch(image, amount: settings.retouch, extent: extent)
        }
        if let mask {
            image = blurBackground(image, mask: mask, amount: settings.blur, extent: extent)
        }
        if settings.filter != .none {
            image = applyColor(settings.filter, amount: settings.filterAmount, to: image, extent: extent)
        }
        if settings.framing {
            image = frame(image, extent: extent, holdFace: started - lastFaceTime < 2)
        } else {
            framingRect = nil
        }
        if settings.mirror {
            image = image.transformed(by: CGAffineTransform(a: -1, b: 0, c: 0, d: 1, tx: extent.width, ty: 0))
        }

        guard let output = makeOutputBuffer(width: width, height: height) else { return pixelBuffer }
        CVBufferPropagateAttachments(pixelBuffer, output)
        context.render(image.cropped(to: extent), to: output, bounds: extent, colorSpace: outputColorSpace)

        let millis = (CACurrentMediaTime() - started) * 1000
        lock.withLock { processingMillis = processingMillis * 0.9 + millis * 0.1 }
        return output
    }

    /// Starts a Vision pass on this frame unless the previous one is still running.
    private func scheduleVision(on pixelBuffer: CVPixelBuffer, mask: Bool, faces: Bool) {
        let start: Bool = lock.withLock {
            if !mask { latestMask = nil }
            if !faces { latestFaces = [] }
            guard (mask || faces) && !visionBusy else { return false }
            visionBusy = true
            return true
        }
        guard start else { return }
        visionQueue.async { [self] in
            var requests: [VNRequest] = []
            if faces { requests.append(faceDetection) }
            if mask { requests.append(segmentation) }
            try? sequenceHandler.perform(requests, on: pixelBuffer, orientation: .up)
            let newMask = mask ? segmentation.results?.first?.pixelBuffer : nil
            let newFaces = faces ? (faceDetection.results ?? []).map(\.boundingBox) : []
            lock.withLock {
                latestMask = newMask
                latestFaces = newFaces
                if !newFaces.isEmpty { lastFaceTime = CACurrentMediaTime() }
                visionBusy = false
            }
        }
    }

    // MARK: - Effects

    /// Soft-focus skin: a blurred copy blended in through soft ellipses over the detected faces.
    private func retouch(_ image: CIImage, amount: Double, extent: CGRect) -> CIImage {
        let smooth = image.clampedToExtent()
            .applyingGaussianBlur(sigma: Double(extent.width) * 0.0035 * amount + 1)
            .cropped(to: extent)
        var mask: CIImage?
        for face in faces {
            let rect = VNImageRectForNormalizedRect(face, Int(extent.width), Int(extent.height))
            let gradient = CIFilter.radialGradient()
            gradient.center = CGPoint(x: rect.midX, y: rect.midY)
            gradient.radius0 = Float(max(rect.width, rect.height) * 0.30)
            gradient.radius1 = Float(max(rect.width, rect.height) * 0.75)
            let strength = CGFloat(min(amount, 1) * 0.85)
            gradient.color0 = CIColor(red: strength, green: strength, blue: strength)
            gradient.color1 = CIColor.black
            guard let faceMask = gradient.outputImage?.cropped(to: extent) else { continue }
            mask = mask.map { faceMask.applyingFilter("CIMaximumCompositing", parameters: [kCIInputBackgroundImageKey: $0]) } ?? faceMask
        }
        guard let mask else { return image }
        let blend = CIFilter.blendWithMask()
        blend.inputImage = smooth
        blend.backgroundImage = image
        blend.maskImage = mask
        return blend.outputImage ?? image
    }

    /// Person stays sharp, everything else gets a lens-like blur (computed at quarter resolution).
    private func blurBackground(_ image: CIImage, mask: CVPixelBuffer, amount: Double, extent: CGRect) -> CIImage {
        let maskImage = CIImage(cvPixelBuffer: mask)
        let scaledMask = maskImage
            .transformed(by: CGAffineTransform(scaleX: extent.width / maskImage.extent.width,
                                               y: extent.height / maskImage.extent.height))
            .clampedToExtent()
            .applyingGaussianBlur(sigma: 2) // soften the cut-out edge
            .cropped(to: extent)

        let downscale: CGFloat = 0.25
        let small = image.transformed(by: CGAffineTransform(scaleX: downscale, y: downscale))
        let sigma = max(2, Double(small.extent.width) * 0.025 * amount)
        let blurred = small.clampedToExtent()
            .applyingGaussianBlur(sigma: sigma)
            .cropped(to: small.extent)
            .transformed(by: CGAffineTransform(scaleX: 1 / downscale, y: 1 / downscale))
            .cropped(to: extent)

        let blend = CIFilter.blendWithMask()
        blend.inputImage = image
        blend.backgroundImage = blurred
        blend.maskImage = scaledMask
        return blend.outputImage ?? image
    }

    private func applyColor(_ filter: ColorFilter, amount: Double, to image: CIImage, extent: CGRect) -> CIImage {
        func channels(_ red: CGFloat, _ green: CGFloat, _ blue: CGFloat, _ input: CIImage) -> CIImage {
            let matrix = CIFilter.colorMatrix()
            matrix.inputImage = input
            matrix.rVector = CIVector(x: red, y: 0, z: 0, w: 0)
            matrix.gVector = CIVector(x: 0, y: green, z: 0, w: 0)
            matrix.bVector = CIVector(x: 0, y: 0, z: blue, w: 0)
            return matrix.outputImage ?? input
        }
        func controls(_ input: CIImage, saturation: Float = 1, contrast: Float = 1, brightness: Float = 0) -> CIImage {
            let filter = CIFilter.colorControls()
            filter.inputImage = input
            filter.saturation = saturation
            filter.contrast = contrast
            filter.brightness = brightness
            return filter.outputImage ?? input
        }

        let styled: CIImage
        switch filter {
        case .none:
            return image
        case .warm:
            styled = controls(channels(1.06, 1.0, 0.90, image), saturation: 1.05)
        case .cool:
            styled = channels(0.93, 1.0, 1.08, image)
        case .cinema: // muted, contrasty, orange skin against teal shadows
            styled = controls(channels(1.05, 0.98, 0.94, image), saturation: 0.85, contrast: 1.12)
        case .vivid:
            let vibrance = CIFilter.vibrance()
            vibrance.inputImage = image
            vibrance.amount = 0.7
            styled = controls(vibrance.outputImage ?? image, contrast: 1.05)
        case .soft:
            let bloom = CIFilter.bloom()
            bloom.inputImage = image
            bloom.radius = Float(extent.width * 0.01)
            bloom.intensity = 0.35
            styled = controls((bloom.outputImage ?? image).cropped(to: extent), saturation: 0.92, contrast: 0.92, brightness: 0.02)
        case .mono:
            styled = image.applyingFilter("CIPhotoEffectMono")
        case .noir:
            styled = image.applyingFilter("CIPhotoEffectNoir")
        }
        guard amount < 0.99 else { return styled }
        return image.applyingFilter("CIDissolveTransition", parameters: [
            kCIInputTargetImageKey: styled,
            kCIInputTimeKey: max(0, amount),
        ])
    }

    /// Digital pan/zoom that keeps the face(s) in the upper-middle third, gliding smoothly.
    private func frame(_ image: CIImage, extent: CGRect, holdFace: Bool) -> CIImage {
        let aspect = extent.width / extent.height
        var target = extent
        if !faces.isEmpty {
            let pixelFaces = faces.map { VNImageRectForNormalizedRect($0, Int(extent.width), Int(extent.height)) }
            let union = pixelFaces.dropFirst().reduce(pixelFaces[0]) { $0.union($1) }
            // Face box covers brows to chin: ~4× its height shows the whole head plus shoulders.
            var height = min(extent.height, max(union.height * 4.0, extent.height / Self.maxFramingZoom))
            var width = height * aspect
            if width < union.width * 1.6 { // several people side by side
                width = min(extent.width, union.width * 1.6)
                height = width / aspect
            }
            if width > extent.width {
                width = extent.width
                height = width / aspect
            }
            // Eyes a little above the middle: the crop's center sits below the face center (origin is bottom-left).
            let center = CGPoint(x: union.midX, y: union.midY - height * 0.06)
            target = CGRect(x: center.x - width / 2, y: center.y - height / 2, width: width, height: height)
            target.origin.x = min(max(target.minX, extent.minX), extent.maxX - width)
            target.origin.y = min(max(target.minY, extent.minY), extent.maxY - height)
        } else if let previous = framingRect, holdFace {
            target = previous // the face vanished briefly (turned away): hold the shot
        }

        let current = framingRect ?? extent
        // Dead zone: ignore tiny moves so the picture doesn't wobble.
        let moved = abs(target.midX - current.midX) + abs(target.midY - current.midY) + abs(target.width - current.width)
        let goal = moved < extent.width * 0.02 ? current : target
        let smoothing: CGFloat = 0.07
        let next = CGRect(x: current.minX + (goal.minX - current.minX) * smoothing,
                          y: current.minY + (goal.minY - current.minY) * smoothing,
                          width: current.width + (goal.width - current.width) * smoothing,
                          height: current.height + (goal.height - current.height) * smoothing)
        framingRect = next

        let scale = extent.width / next.width
        return image.cropped(to: next)
            .transformed(by: CGAffineTransform(translationX: -next.minX, y: -next.minY)
                .concatenating(CGAffineTransform(scaleX: scale, y: scale)))
    }

    // MARK: - Buffers

    private func makeOutputBuffer(width: Int, height: Int) -> CVPixelBuffer? {
        let size = CGSize(width: width, height: height)
        if pool == nil || poolSize != size {
            let attributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
                kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
                kCVPixelBufferMetalCompatibilityKey as String: true,
            ]
            pool = nil
            CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &pool)
            poolSize = size
        }
        guard let pool else { return nil }
        var buffer: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &buffer)
        return buffer
    }
}
