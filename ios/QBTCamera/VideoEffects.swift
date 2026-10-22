import AVFoundation
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
    var background = BackgroundMode.off
    /// Portrait blur strength (like the aperture), 0…1.
    var blur = 0.6
    var backdrop = Backdrop.studio
    /// Chroma key: how eagerly the screen color disappears, how soft the edge is, how much
    /// green/blue cast is removed from hair and skin. All 0…1.
    var keySensitivity = 0.5
    var keySoftness = 0.4
    var keySpill = 0.7
    /// Skin smoothing strength, 0 = off.
    var retouch = 0.0
    /// Keep the face(s) framed by cropping in, like Center Stage.
    var framing = false
    var filter = ColorFilter.none
    var filterAmount = 1.0
    /// Flip left-right before sending.
    var mirror = false
    /// Background effects computed by the PC's GPU (the phone sends a small matte stream) instead of here.
    var processOnPC = false

    var isActive: Bool { background != .off || retouch > 0.01 || framing || filter != .none || mirror }
    /// Depth (LiDAR / TrueDepth) makes these look like a real lens; the camera switches to a depth device.
    var wantsDepth: Bool { background == .portrait || background == .replace }
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
    // Face rectangles (normalized, bottom-left origin), guarded by `lock`.
    private var latestFaces: [CGRect] = []
    private var lastFaceTime = 0.0
    private var lastVisionFaceCheck = 0.0
    private var ispFacesAvailable = false

    private let context: CIContext
    private let outputColorSpace = CGColorSpace(name: CGColorSpace.itur_709)!
    private let faceDetection = VNDetectFaceRectanglesRequest()
    // The person mask is computed on the very frame being sent (a late mask leaves trails when
    // you move).
    private let liveSegmentation: VNGeneratePersonSegmentationRequest = {
        let request = VNGeneratePersonSegmentationRequest()
        request.qualityLevel = .balanced
        request.outputPixelFormat = kCVPixelFormatType_OneComponent8
        return request
    }()
    private let coolSegmentation: VNGeneratePersonSegmentationRequest = {
        let request = VNGeneratePersonSegmentationRequest()
        request.qualityLevel = .fast
        request.outputPixelFormat = kCVPixelFormatType_OneComponent8
        return request
    }()

    private var pool: CVPixelBufferPool?
    private var poolSize = CGSize.zero
    private var faces: [CGRect] = [] // snapshot of latestFaces for the frame being rendered
    private var framingRect: CGRect?
    private var processingMillis = 0.0
    private var framesInDelivery = 0
    private var lastDelivery = CACurrentMediaTime()
    private let deliveryQueue = DispatchQueue(label: "qbt.effects.delivery", qos: .userInteractive)
    private var visionMillis = 0.0
    private var auxPool: CVPixelBufferPool?
    private var auxPoolSize = CGSize.zero
    /// Matte frames for PC-side processing, called on the delivery queue right before the picture.
    var onAuxFrame: ((CVPixelBuffer, CMTime) -> Void)?
    private let background: BackgroundEngine

    init() {
        if let device = MTLCreateSystemDefaultDevice() {
            context = CIContext(mtlDevice: device, options: [.cacheIntermediates: false])
        } else {
            context = CIContext(options: [.cacheIntermediates: false])
        }
        background = BackgroundEngine(context: context)
    }

    /// Depth from the camera (LiDAR / TrueDepth), any thread.
    func updateDepth(_ depth: AVDepthData?) {
        background.setDepth(depth)
    }

    /// Hardware ISP face rectangles from `AVCaptureMetadataOutput` (0% GPU/ANE cost), or `nil` when ISP face detection is inactive.
    func updateFaces(_ newFaces: [CGRect]?) {
        let now = CACurrentMediaTime()
        lock.withLock {
            if let newFaces {
                ispFacesAvailable = true
                latestFaces = newFaces
                if !newFaces.isEmpty { lastFaceTime = now }
            } else {
                ispFacesAvailable = false
                latestFaces = []
            }
        }
    }

    var hasDepth: Bool { background.hasDepth }

    func resampleKeyColor() {
        background.resampleKeyColor()
    }

    func update(_ newSettings: EffectSettings) {
        lock.withLock { settings = newSettings }
    }

    var current: EffectSettings { lock.withLock { settings } }

    /// Average GPU+Vision time per processed frame, for the PC's status line.
    var lastProcessingMillis: Double { lock.withLock { processingMillis } }
    /// Part of it spent waiting for the person mask.
    var lastVisionMillis: Double { lock.withLock { visionMillis } }

    /// Called on the capture queue for every frame; `deliver` gets the result (the input itself when
    /// no effect is on) on the delivery queue, always in capture order.
    ///
    /// Critically, all Vision and Metal GPU reads of `pixelBuffer` complete before `process` returns on
    /// the capture queue so `AVCaptureVideoDataOutput` never recycles the camera `IOSurface` back to the
    /// ISP while a GPU or Neural Engine `IOFence` is still waiting on it.
    func process(_ pixelBuffer: CVPixelBuffer, pts: CMTime, deliver: @escaping (CVPixelBuffer, CMTime) -> Void) {
        let settings = current
        guard settings.isActive else {
            framingRect = nil
            deliveryQueue.async { deliver(pixelBuffer, pts) }
            return
        }
        let canDeliver: Bool = lock.withLock {
            if framesInDelivery >= 2 && CACurrentMediaTime() - lastDelivery > 2 {
                framesInDelivery = 0
            }
            return framesInDelivery < 2
        }
        guard canDeliver else { return }
        let started = CACurrentMediaTime()
        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        let extent = CGRect(x: 0, y: 0, width: width, height: height)
        let output16x9 = Self.sixteenByNine(extent)
        // On the PC: the picture leaves without the background effect, plus a small matte stream;
        // the PC's GPU refines and composites. The phone keeps only the Neural Engine work (cool).
        let remote = settings.processOnPC && settings.background != .off && onAuxFrame != nil

        // 1. Faces from the camera ISP (`AVCaptureMetadataOutput`), falling back to throttled (8 Hz)
        // stateless Vision on the capture queue only if the active format doesn't emit ISP face metadata.
        let wantsMask = settings.background == .portrait || settings.background == .replace
        let wantsFaces = settings.framing || settings.retouch > 0.01 || settings.background == .portrait
        let useVisionFaces: Bool = lock.withLock {
            guard wantsFaces, !ispFacesAvailable, started - lastVisionFaceCheck >= 0.125 else { return false }
            lastVisionFaceCheck = started
            return true
        }
        if useVisionFaces {
            try? VNImageRequestHandler(cvPixelBuffer: pixelBuffer, orientation: .up).perform([faceDetection])
            let detected = (faceDetection.results ?? []).map(\.boundingBox)
            let now = CACurrentMediaTime()
            lock.withLock {
                latestFaces = detected
                if !detected.isEmpty { lastFaceTime = now }
            }
        }
        let lastFaceTime: Double
        (faces, lastFaceTime) = lock.withLock { (wantsFaces ? latestFaces : [], self.lastFaceTime) }
        var mask: CVPixelBuffer?
        if wantsMask {
            let visionStarted = CACurrentMediaTime()
            // A fresh handler per frame: VNSequenceRequestHandler smooths masks over time, which drags
            // the mask behind a moving hand. Our own edge-aware smoothing handles flicker instead.
            // A hot iPhone gets the lighter mask: iOS would otherwise start throttling the camera.
            let hot = ProcessInfo.processInfo.thermalState.rawValue >= ProcessInfo.ThermalState.serious.rawValue
            let request = hot || Self.olderChip ? coolSegmentation : liveSegmentation
            try? VNImageRequestHandler(cvPixelBuffer: pixelBuffer, orientation: .up).perform([request])
            mask = request.results?.first?.pixelBuffer
            let visionTime = (CACurrentMediaTime() - visionStarted) * 1000
            lock.withLock { visionMillis = visionMillis * 0.9 + visionTime * 0.1 }
        }

        // 2. Core Image chain: retouch → background → color → geometry.
        var image = CIImage(cvPixelBuffer: pixelBuffer)
        if settings.retouch > 0.01, !faces.isEmpty {
            image = retouch(image, amount: settings.retouch, extent: extent)
        }
        var aux: CIImage?
        if settings.background != .off {
            if !remote {
                image = background.render(image, extent: extent, settings: settings, personMask: mask, faces: faces)
            } else if settings.background != .chroma, // the PC keys green screens by itself
                      let coarse = background.coarseMatte(extent: extent, settings: settings, personMask: mask, faces: faces) {
                aux = coarse.transformed(by: CGAffineTransform(scaleX: extent.width / coarse.extent.width,
                                                                y: extent.height / coarse.extent.height))
            }
        }
        if settings.filter != .none {
            image = applyColor(settings.filter, amount: settings.filterAmount, to: image, extent: extent)
        }

        // Geometry — face framing, mirror, 16:9 — identical for the picture and the matte.
        let framing = settings.framing ? nextFramingRect(extent: extent, holdFace: started - lastFaceTime < 2) : nil
        if !settings.framing { framingRect = nil }
        func geometry(_ input: CIImage) -> CIImage {
            var result = input
            if let framing {
                let scaleX = extent.width / framing.width
                let scaleY = extent.height / framing.height
                result = result.cropped(to: framing)
                    .transformed(by: CGAffineTransform(translationX: -framing.minX, y: -framing.minY)
                        .concatenating(CGAffineTransform(scaleX: scaleX, y: scaleY)))
                    .cropped(to: extent)
            }
            if settings.mirror {
                result = result.transformed(by: CGAffineTransform(a: -1, b: 0, c: 0, d: 1, tx: extent.width, ty: 0))
            }
            // 4:3 depth formats (LiDAR) leave as 16:9: the middle of the picture.
            if output16x9 != extent {
                result = result.cropped(to: output16x9)
                    .transformed(by: CGAffineTransform(translationX: -output16x9.minX, y: -output16x9.minY))
            }
            return result
        }
        image = geometry(image)

        let outputExtent = CGRect(origin: .zero, size: output16x9.size)
        guard let output = makeOutputBuffer(width: Int(outputExtent.width), height: Int(outputExtent.height)) else {
            deliveryQueue.async { deliver(pixelBuffer, pts) }
            return
        }
        CVBufferSetAttachment(output, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(output, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(output, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, .shouldPropagate)
        let destination = CIRenderDestination(pixelBuffer: output)
        destination.colorSpace = outputColorSpace
        guard let task = try? context.startTask(toRender: image.cropped(to: outputExtent), from: outputExtent, to: destination,
                                                at: .zero) else {
            deliveryQueue.async { deliver(pixelBuffer, pts) }
            return
        }

        // The matte stream: [foreground | blur amount] side by side, gray, quarter resolution.
        var auxRender: (task: CIRenderTask, buffer: CVPixelBuffer)?
        if let aux {
            let quarterWidth = CGFloat(Int(outputExtent.width / 4) & ~1), quarterHeight = CGFloat(Int(outputExtent.height / 4) & ~1)
            let shaped = geometry(aux).transformed(by: CGAffineTransform(scaleX: quarterWidth / outputExtent.width,
                                                                          y: quarterHeight / outputExtent.height))
            let quarter = CGRect(x: 0, y: 0, width: quarterWidth, height: quarterHeight)
            func gray(_ channel: CIVector) -> CIImage {
                shaped.applyingFilter("CIColorMatrix", parameters: ["inputRVector": channel, "inputGVector": channel,
                                                                    "inputBVector": channel,
                                                                    "inputAVector": CIVector(x: 0, y: 0, z: 0, w: 1)])
                    .cropped(to: quarter)
            }
            let foreground = gray(CIVector(x: 1, y: 0, z: 0, w: 0))
            let blur = gray(CIVector(x: 0, y: 1, z: 0, w: 0)).transformed(by: CGAffineTransform(translationX: quarterWidth, y: 0))
            let packedExtent = CGRect(x: 0, y: 0, width: quarterWidth * 2, height: quarterHeight)
            if let buffer = makeAuxBuffer(width: Int(packedExtent.width), height: Int(packedExtent.height)) {
                let auxDestination = CIRenderDestination(pixelBuffer: buffer)
                auxDestination.colorSpace = nil // raw values: 0…1 → 0…255
                if let auxTask = try? context.startTask(toRender: blur.composited(over: foreground), from: packedExtent,
                                                         to: auxDestination, at: .zero) {
                    auxRender = (auxTask, buffer)
                }
            }
        }

        // Wait for the GPU render to complete while `pixelBuffer` and `mask` are still strongly retained
        // on the capture queue, preventing `IOSurface` recycling while in flight on the GPU.
        withExtendedLifetime((pixelBuffer, mask)) {
            _ = try? task.waitUntilCompleted()
            if let auxRender {
                _ = try? auxRender.task.waitUntilCompleted()
            }
        }
        let millis = (CACurrentMediaTime() - started) * 1000
        lock.withLock {
            framesInDelivery += 1
            processingMillis = processingMillis * 0.9 + millis * 0.1
        }
        let auxBuffer = auxRender?.buffer
        deliveryQueue.async { [self] in
            if let auxBuffer {
                onAuxFrame?(auxBuffer, pts) // before the picture: the PC pairs them by timestamp
            }
            lock.withLock {
                framesInDelivery = max(0, framesInDelivery - 1)
                lastDelivery = CACurrentMediaTime()
            }
            deliver(output, pts)
        }
    }

    /// Full-range NV12 buffers for the matte stream.
    private func makeAuxBuffer(width: Int, height: Int) -> CVPixelBuffer? {
        let size = CGSize(width: width, height: height)
        if auxPool == nil || auxPoolSize != size {
            let attributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
                kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
                kCVPixelBufferMetalCompatibilityKey as String: true,
            ]
            auxPool = nil
            CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &auxPool)
            auxPoolSize = size
        }
        guard let auxPool else { return nil }
        var buffer: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, auxPool, &buffer)
        return buffer
    }

    /// A12/A13 iPhones (XS, XR, 11, SE 2nd gen — the oldest that run iOS 18) get the lighter person mask
    /// so 30 fps holds; the guided filter still restores the fine edge.
    private static let olderChip: Bool = {
        var system = utsname()
        uname(&system)
        let machine = withUnsafeBytes(of: system.machine) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        // "iPhone11,8" (XR) … "iPhone12,8" (SE 2): the major number is below 13 (iPhone 12 = "iPhone13,x").
        guard machine.hasPrefix("iPhone"), let major = Int(machine.dropFirst(6).prefix { $0 != "," }) else { return false }
        return major < 13
    }()

    /// The centered 16:9 (or 9:16) part of a frame; the frame itself when it already is.
    private static func sixteenByNine(_ extent: CGRect) -> CGRect {
        let landscape = extent.width >= extent.height
        let long = max(extent.width, extent.height), short = min(extent.width, extent.height)
        let wantedShort = (long * 9 / 16).rounded(.down) - ((long * 9 / 16).rounded(.down)).truncatingRemainder(dividingBy: 2)
        guard abs(short - wantedShort) > 2 else { return extent }
        if landscape {
            return CGRect(x: 0, y: ((extent.height - wantedShort) / 2).rounded(), width: extent.width, height: wantedShort)
        }
        return CGRect(x: ((extent.width - wantedShort) / 2).rounded(), y: 0, width: wantedShort, height: extent.height)
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
    private func nextFramingRect(extent: CGRect, holdFace: Bool) -> CGRect {
        let aspect = extent.width / extent.height
        let current: CGRect
        if let previous = framingRect, previous.height > 0, abs(previous.width / previous.height - aspect) < 0.05 {
            current = previous
        } else {
            current = extent
        }
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
        } else if holdFace {
            target = current // the face vanished briefly (turned away): hold the shot
        }

        // Dead zone: ignore tiny moves so the picture doesn't wobble.
        let moved = abs(target.midX - current.midX) + abs(target.midY - current.midY) + abs(target.width - current.width)
        let goal = moved < extent.width * 0.02 ? current : target
        let smoothing: CGFloat = 0.07
        var next = CGRect(x: current.minX + (goal.minX - current.minX) * smoothing,
                          y: current.minY + (goal.minY - current.minY) * smoothing,
                          width: current.width + (goal.width - current.width) * smoothing,
                          height: current.height + (goal.height - current.height) * smoothing)
        next.size.width = min(next.width, extent.width)
        next.size.height = min(next.height, extent.height)
        next.origin.x = min(max(next.minX, extent.minX), extent.maxX - next.width)
        next.origin.y = min(max(next.minY, extent.minY), extent.maxY - next.height)
        framingRect = next
        return next
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
