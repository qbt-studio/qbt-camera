import AVFoundation
import CoreImage
import CoreImage.CIFilterBuiltins
import QuartzCore

enum BackgroundMode: String, CaseIterable, Identifiable {
    case off
    /// Lens-like blur that follows the depth map (LiDAR / TrueDepth), or the person mask without depth.
    case portrait
    /// The subject over a QBT backdrop.
    case replace
    /// Green (or any color) screen keying.
    case chroma

    var id: String { rawValue }

    var title: String {
        switch self {
        case .off: "Выкл"
        case .portrait: "Портрет"
        case .replace: "Замена"
        case .chroma: "Хромакей"
        }
    }
}

enum Backdrop: String, CaseIterable, Identifiable {
    case studio, glow, warm, white, black, green

    var id: String { rawValue }

    var title: String {
        switch self {
        case .studio: "Студия"
        case .glow: "QBT"
        case .warm: "Тёплый"
        case .white: "Белый"
        case .black: "Чёрный"
        case .green: "Зелёный для OBS"
        }
    }
}

/// Background separation and compositing on the GPU.
///
/// Matte: Vision's person mask (a few frames old, low resolution) plus — when the camera has it —
/// depth from LiDAR or TrueDepth, refined against the current frame with a guided filter so the
/// edge follows hair and fingers, and blended over time so it doesn't shimmer.
/// Portrait: the background is blurred in three levels from a premultiplied "background only"
/// image (no halo around the person) and the level follows the depth, like a real lens.
final class BackgroundEngine {
    private let context: CIContext
    private let kernels: Kernels?

    private let lock = NSLock()
    private var depth: CVPixelBuffer?   // DepthFloat16, meters, guarded by `lock`
    private var depthTime = 0.0

    // Matte history (rendered every frame; the previous one smooths the next).
    private var mattePool: CVPixelBufferPool?
    private var matteSize = CGSize.zero
    private var previousMatte: CVPixelBuffer?

    private var focusDistance: Float = 0.8 // meters, smoothed
    private var keyColor: CIVector?
    private var keySampleRequested = true
    private var loggedDepthMismatch = false

    private struct Kernels {
        let depthMap: CIColorKernel
        let coarseMatte: CIColorKernel
        let guidePrep: CIColorKernel
        let guideCoefficients: CIColorKernel
        let guideApply: CIColorKernel
        let guideColorA: CIColorKernel
        let guideColorB: CIColorKernel
        let guideColorC: CIColorKernel
        let guideColorD: CIColorKernel
        let guideColorCoefficients: CIColorKernel
        let guideColorApply: CIColorKernel
        let backgroundOnly: CIColorKernel
        let portrait: CIColorKernel
        let replace: CIColorKernel
        let chromaKey: CIColorKernel

        init?() {
            guard let url = Bundle.main.url(forResource: "default", withExtension: "metallib"),
                  let data = try? Data(contentsOf: url) else { return nil }
            func kernel(_ name: String) -> CIColorKernel? { try? CIColorKernel(functionName: name, fromMetalLibraryData: data) }
            guard let depthMap = kernel("qbtDepthMap"), let coarseMatte = kernel("qbtCoarseMatte"),
                  let guidePrep = kernel("qbtGuidePrep"), let guideCoefficients = kernel("qbtGuideCoefficients"),
                  let guideApply = kernel("qbtGuideApply"), let backgroundOnly = kernel("qbtBackgroundOnly"),
                  let portrait = kernel("qbtPortrait"), let replace = kernel("qbtReplace"),
                  let chromaKey = kernel("qbtChromaKey"),
                  let guideColorA = kernel("qbtGuideColorA"), let guideColorB = kernel("qbtGuideColorB"),
                  let guideColorC = kernel("qbtGuideColorC"), let guideColorD = kernel("qbtGuideColorD"),
                  let guideColorCoefficients = kernel("qbtGuideColorCoefficients"),
                  let guideColorApply = kernel("qbtGuideColorApply") else { return nil }
            self.guideColorA = guideColorA
            self.guideColorB = guideColorB
            self.guideColorC = guideColorC
            self.guideColorD = guideColorD
            self.guideColorCoefficients = guideColorCoefficients
            self.guideColorApply = guideColorApply
            self.depthMap = depthMap
            self.coarseMatte = coarseMatte
            self.guidePrep = guidePrep
            self.guideCoefficients = guideCoefficients
            self.guideApply = guideApply
            self.backgroundOnly = backgroundOnly
            self.portrait = portrait
            self.replace = replace
            self.chromaKey = chromaKey
        }
    }

    init(context: CIContext) {
        self.context = context
        kernels = Kernels()
        if kernels == nil { print("QBT: background kernels missing from the app bundle") }
    }

    /// Newest depth from the camera (any thread); nil when the camera has no depth.
    func setDepth(_ depthData: AVDepthData?) {
        let converted = depthData.map {
            $0.depthDataType == kCVPixelFormatType_DepthFloat16 ? $0 : $0.converting(toDepthDataType: kCVPixelFormatType_DepthFloat16)
        }
        lock.withLock {
            depth = converted?.depthDataMap
            depthTime = CACurrentMediaTime()
        }
    }

    /// Next chroma-key frame samples the screen color from the picture's edges again.
    func resampleKeyColor() {
        lock.withLock { keySampleRequested = true }
    }

    /// True when depth arrived recently (shown in the UI as "LiDAR" / "TrueDepth").
    var hasDepth: Bool {
        lock.withLock { depth != nil && CACurrentMediaTime() - depthTime < 1 }
    }

    /// Called on the capture queue. `personMask` is Vision's newest mask, `faces` normalized rects.
    func render(_ image: CIImage, extent: CGRect, settings: EffectSettings, personMask: CVPixelBuffer?,
                faces: [CGRect]) -> CIImage {
        guard let kernels, settings.background != .off else {
            previousMatte = nil
            return image
        }
        if settings.background == .chroma {
            previousMatte = nil
            return chromaKey(image, extent: extent, settings: settings, kernels: kernels)
        }

        let usableDepth = usableDepth(for: extent)
        guard personMask != nil || usableDepth != nil else { return image } // nothing to separate yet

        guard let matte = makeMatte(image, extent: extent, personMask: personMask, depth: usableDepth, faces: faces,
                                    replaceMode: settings.background == .replace, kernels: kernels) else { return image }

        switch settings.background {
        case .replace:
            let backdrop = Self.backdrop(settings.backdrop, extent: extent)
            let estimate = backgroundLevels(image, matte: matte, extent: extent, sigmas: [3], kernels: kernels).first ?? image
            return kernels.replace.apply(extent: extent, arguments: [image, matte, backdrop, estimate, 0.12]) ?? image
        default:
            return portrait(image, matte: matte, extent: extent, strength: settings.blur, kernels: kernels)
        }
    }

    // MARK: - Matte

    private func makeMatte(_ image: CIImage, extent: CGRect, personMask: CVPixelBuffer?, depth: CVPixelBuffer?,
                           faces: [CGRect], replaceMode: Bool, kernels: Kernels) -> CIImage? {
        func fit(_ input: CIImage, to target: CGRect) -> CIImage {
            input.transformed(by: CGAffineTransform(scaleX: target.width / input.extent.width,
                                                    y: target.height / input.extent.height))
                .clampedToExtent().cropped(to: target)
        }

        guard let coarse = coarse(personMask: personMask, depth: depth, faces: faces, extent: extent, replaceMode: replaceMode,
                                  kernels: kernels) else { return nil }

        // Color guided filter at half resolution: the matte snaps to the real edges of this frame,
        // separating by color (hair vs. a wall of the same brightness). Edges follow single hairs and
        // the GPU still touches only a quarter of the pixels.
        let half = CGRect(x: 0, y: 0, width: (extent.width / 2).rounded(), height: (extent.height / 2).rounded())
        let halfImage = fit(image, to: half)
        let halfCoarse = fit(coarse, to: half)
        let radius = max(4.0, Double(half.width) / 110)
        func boxMean(_ input: CIImage?) -> CIImage? {
            input?.clampedToExtent().applyingFilter("CIBoxBlur", parameters: [kCIInputRadiusKey: radius]).cropped(to: half)
        }
        guard let meanA = boxMean(kernels.guideColorA.apply(extent: half, arguments: [halfImage, halfCoarse])),
              let meanB = boxMean(kernels.guideColorB.apply(extent: half, arguments: [halfImage])),
              let meanC = boxMean(kernels.guideColorC.apply(extent: half, arguments: [halfImage, halfCoarse])),
              let meanD = boxMean(kernels.guideColorD.apply(extent: half, arguments: [halfImage, halfCoarse])),
              let coefficientMeans = boxMean(kernels.guideColorCoefficients.apply(extent: half,
                                                                                  arguments: [meanA, meanB, meanC, meanD, 0.0004]))
        else { return nil }

        let previous = previousMatte.map { CIImage(cvPixelBuffer: $0, options: [.colorSpace: NSNull()]) }
        let previousImage = previous.flatMap { $0.extent.size == half.size ? $0 : nil } ?? halfCoarse
        let keep = previous == nil ? 0.0 : 0.4
        guard let refined = kernels.guideColorApply.apply(extent: half,
                                                           arguments: [halfImage, coefficientMeans, halfCoarse, previousImage, keep])
        else { return nil }

        // Render the matte so the next frame can blend with it. Queued without waiting: the final
        // render runs on the same GPU queue right after it.
        let upscale = CGAffineTransform(scaleX: extent.width / half.width, y: extent.height / half.height)
        guard let buffer = makeMatteBuffer(size: half.size) else { return refined.transformed(by: upscale) }
        let destination = CIRenderDestination(pixelBuffer: buffer)
        destination.colorSpace = nil
        guard (try? context.startTask(toRender: refined, to: destination)) != nil else {
            return refined.transformed(by: upscale)
        }
        previousMatte = buffer
        return CIImage(cvPixelBuffer: buffer, options: [.colorSpace: NSNull()]).transformed(by: upscale).cropped(to: extent)
    }

    /// For processing on the PC: the coarse (foreground, blur amount) at quarter resolution, from the
    /// person mask and depth exactly as the phone's own matte starts. The PC refines and composites.
    func coarseMatte(extent: CGRect, settings: EffectSettings, personMask: CVPixelBuffer?, faces: [CGRect]) -> CIImage? {
        guard let kernels else { return nil }
        let depth = usableDepth(for: extent)
        guard personMask != nil || depth != nil else { return nil }
        return coarse(personMask: personMask, depth: depth, faces: faces, extent: extent,
                      replaceMode: settings.background == .replace, kernels: kernels)
    }

    /// Coarse foreground at quarter resolution: person ∪ in-focus depth (R), blur amount (G).
    private func coarse(personMask: CVPixelBuffer?, depth: CVPixelBuffer?, faces: [CGRect], extent: CGRect, replaceMode: Bool,
                        kernels: Kernels) -> CIImage? {
        let quarter = CGRect(x: 0, y: 0, width: (extent.width / 4).rounded(), height: (extent.height / 4).rounded())
        let person = personMask.map { Self.fit(CIImage(cvPixelBuffer: $0), to: quarter) } ?? CIImage(color: .black).cropped(to: quarter)
        var depthMap = CIImage(color: .black).cropped(to: quarter)
        if let depth {
            updateFocus(depth: depth, faces: faces)
            let depthImage = Self.fit(CIImage(cvPixelBuffer: depth), to: quarter)
            depthMap = kernels.depthMap.apply(extent: quarter, arguments: [depthImage, focusDistance]) ?? depthMap
        }
        // Portrait keeps the whole focus plane sharp, like a real lens. Replace cuts out the person only:
        // there depth just vetoes stale mask regions (anything else at that distance — a desk,
        // a curtain — goes with the background).
        let nearPerson = CIImage(color: replaceMode ? .black : .white).cropped(to: quarter)
        guard let coarse = kernels.coarseMatte.apply(extent: quarter,
                                                      arguments: [person, depthMap, nearPerson, depth != nil ? 1.0 : 0.0])
        else { return nil }
        return coarse.clampedToExtent().applyingGaussianBlur(sigma: 1.2).cropped(to: quarter)
    }

    private static func fit(_ input: CIImage, to target: CGRect) -> CIImage {
        input.transformed(by: CGAffineTransform(scaleX: target.width / input.extent.width, y: target.height / input.extent.height))
            .clampedToExtent().cropped(to: target)
    }

    /// Recent depth whose orientation matches the picture (else nil: mask only).
    private func usableDepth(for extent: CGRect) -> CVPixelBuffer? {
        let depthBuffer: CVPixelBuffer? = lock.withLock { CACurrentMediaTime() - depthTime < 1 ? depth : nil }
        return depthBuffer.flatMap { buffer -> CVPixelBuffer? in
            let landscapeDepth = CVPixelBufferGetWidth(buffer) >= CVPixelBufferGetHeight(buffer)
            if landscapeDepth == (extent.width >= extent.height) { return buffer }
            if !loggedDepthMismatch {
                loggedDepthMismatch = true
                print("QBT: depth map orientation differs from video, using the person mask only")
            }
            return nil
        }
    }

    /// Focus on the face (or the middle of the picture), smoothed like a camera's focus pull.
    private func updateFocus(depth: CVPixelBuffer, faces: [CGRect]) {
        let center: CGPoint
        if let face = faces.max(by: { $0.width * $0.height < $1.width * $1.height }) {
            center = CGPoint(x: face.midX, y: face.midY)
        } else {
            center = CGPoint(x: 0.5, y: 0.5)
        }
        guard let measured = Self.medianDepth(depth, around: center), measured > 0.1, measured < 8 else { return }
        focusDistance += (measured - focusDistance) * 0.2
    }

    /// Median of a small window around a normalized (bottom-left origin) point.
    private static func medianDepth(_ buffer: CVPixelBuffer, around point: CGPoint) -> Float? {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return nil }
        let width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer)
        let rowBytes = CVPixelBufferGetBytesPerRow(buffer)
        let cx = Int(point.x * CGFloat(width)), cy = Int((1 - point.y) * CGFloat(height))
        let half = max(2, width / 64)
        var values: [Float] = []
        for y in max(0, cy - half)...min(height - 1, cy + half) {
            let row = base.advanced(by: y * rowBytes).assumingMemoryBound(to: Float16.self)
            for x in max(0, cx - half)...min(width - 1, cx + half) {
                let value = Float(row[x])
                if value.isFinite, value > 0.05 { values.append(value) }
            }
        }
        guard !values.isEmpty else { return nil }
        values.sort()
        return values[values.count / 2]
    }

    private func makeMatteBuffer(size: CGSize) -> CVPixelBuffer? {
        if mattePool == nil || matteSize != size {
            let attributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                kCVPixelBufferWidthKey as String: Int(size.width),
                kCVPixelBufferHeightKey as String: Int(size.height),
                kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
                kCVPixelBufferMetalCompatibilityKey as String: true,
            ]
            mattePool = nil
            previousMatte = nil
            CVPixelBufferPoolCreate(nil, [kCVPixelBufferPoolMinimumBufferCountKey: 3] as CFDictionary, attributes as CFDictionary,
                                    &mattePool)
            matteSize = size
        }
        guard let mattePool else { return nil }
        var buffer: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, mattePool, &buffer)
        return buffer
    }

    // MARK: - Portrait

    private func portrait(_ image: CIImage, matte: CIImage, extent: CGRect, strength: Double, kernels: Kernels) -> CIImage {
        let amount = min(max(strength, 0.05), 1)
        let levels = backgroundLevels(image, matte: matte, extent: extent,
                                      sigmas: [1 + 3 * amount, 2 + 8 * amount, 4 + 18 * amount], kernels: kernels)
        guard levels.count == 3 else { return image }
        return kernels.portrait.apply(extent: extent, arguments: [image, matte, levels[0], levels[1], levels[2], 0.25]) ?? image
    }

    /// The background without the person, blurred by each sigma (premultiplied: divide by alpha).
    /// Computed at quarter resolution; sigmas are in quarter-resolution pixels of a 1080p picture.
    private func backgroundLevels(_ image: CIImage, matte: CIImage, extent: CGRect, sigmas: [Double],
                                  kernels: Kernels) -> [CIImage] {
        let quarter = CGRect(x: 0, y: 0, width: (extent.width / 4).rounded(), height: (extent.height / 4).rounded())
        let down = CGAffineTransform(scaleX: quarter.width / extent.width, y: quarter.height / extent.height)
        let up = down.inverted()
        guard let backgroundOnly = kernels.backgroundOnly.apply(extent: quarter,
                                                                 arguments: [image.transformed(by: down), matte.transformed(by: down)])
        else { return [] }
        // Blur radii scale with the picture so 720p, 1080p and 4K look the same.
        let unit = Double(min(quarter.width, quarter.height)) / 270
        return sigmas.map { sigma in
            backgroundOnly.clampedToExtent()
                .applyingGaussianBlur(sigma: sigma * unit)
                .cropped(to: quarter)
                .transformed(by: up)
                .cropped(to: extent)
        }
    }

    // MARK: - Chroma key

    private func chromaKey(_ image: CIImage, extent: CGRect, settings: EffectSettings, kernels: Kernels) -> CIImage {
        if lock.withLock({ keySampleRequested }) || keyColor == nil {
            keyColor = sampleKeyColor(image, extent: extent) ?? CIVector(x: 0.2, y: 0.75, z: 0.3)
            lock.withLock { keySampleRequested = false }
        }
        let backdrop = Self.backdrop(settings.backdrop, extent: extent)
        // Sensitivity 0…1 → how far towards the key a pixel must lean to disappear.
        let threshold = 0.42 - 0.34 * settings.keySensitivity
        let softness = 0.06 + 0.3 * settings.keySoftness
        return kernels.chromaKey.apply(extent: extent, arguments: [image, backdrop, keyColor!, threshold, softness,
                                                                    settings.keySpill, 0.3]) ?? image
    }

    /// The screen's color: the most saturated pixels along the top and side edges of a thumbnail.
    private func sampleKeyColor(_ image: CIImage, extent: CGRect) -> CIVector? {
        let width = 64, height = 36
        let thumbnail = image.transformed(by: CGAffineTransform(scaleX: CGFloat(width) / extent.width,
                                                                y: CGFloat(height) / extent.height))
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        context.render(thumbnail, toBitmap: &pixels, rowBytes: width * 4, bounds: CGRect(x: 0, y: 0, width: width, height: height),
                       format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.linearSRGB)) // the kernels work in linear light
        var samples: [(saturation: Float, color: SIMD3<Float>)] = []
        func add(_ x: Int, _ y: Int) {
            let i = (y * width + x) * 4
            let color = SIMD3<Float>(Float(pixels[i]), Float(pixels[i + 1]), Float(pixels[i + 2])) / 255
            samples.append((color.max() - color.min(), color))
        }
        for x in 0..<width { add(x, 0); add(x, 1) }      // top rows (bitmap row 0 is the top)
        for y in 0..<height * 2 / 3 { add(0, y); add(1, y); add(width - 1, y); add(width - 2, y) }
        samples.sort { $0.saturation > $1.saturation }
        let best = samples.prefix(max(8, samples.count / 3))
        guard let first = best.first, first.saturation > 0.12 else { return nil } // no colored screen there
        let average = best.reduce(SIMD3<Float>.zero) { $0 + $1.color } / Float(best.count)
        return CIVector(x: CGFloat(average.x), y: CGFloat(average.y), z: CGFloat(average.z))
    }

    // MARK: - Backdrops

    static func backdrop(_ backdrop: Backdrop, extent: CGRect) -> CIImage {
        func radial(_ inner: CIColor, _ outer: CIColor, center: CGPoint, radius: CGFloat) -> CIImage {
            let gradient = CIFilter.radialGradient()
            gradient.center = center
            gradient.radius0 = 0
            gradient.radius1 = Float(radius)
            gradient.color0 = inner
            gradient.color1 = outer
            return gradient.outputImage?.cropped(to: extent) ?? CIImage(color: outer).cropped(to: extent)
        }
        func linear(_ top: CIColor, _ bottom: CIColor) -> CIImage {
            let gradient = CIFilter.linearGradient()
            gradient.point0 = CGPoint(x: 0, y: extent.height)
            gradient.point1 = CGPoint(x: 0, y: 0)
            gradient.color0 = top
            gradient.color1 = bottom
            return gradient.outputImage?.cropped(to: extent) ?? CIImage(color: bottom).cropped(to: extent)
        }
        let center = CGPoint(x: extent.midX, y: extent.height * 0.62)
        let reach = max(extent.width, extent.height) * 0.75
        switch backdrop {
        case .studio: // soft key light on a dark grey paper backdrop
            return radial(CIColor(red: 0.30, green: 0.33, blue: 0.38), CIColor(red: 0.07, green: 0.08, blue: 0.10),
                          center: center, radius: reach)
        case .glow: // QBT blue
            return radial(CIColor(red: 0.20, green: 0.45, blue: 0.95), CIColor(red: 0.03, green: 0.05, blue: 0.16),
                          center: center, radius: reach)
        case .warm: // warm living-room wall
            return radial(CIColor(red: 0.86, green: 0.77, blue: 0.66), CIColor(red: 0.52, green: 0.42, blue: 0.33),
                          center: center, radius: reach)
        case .white:
            return linear(CIColor(red: 0.97, green: 0.97, blue: 0.98), CIColor(red: 0.84, green: 0.85, blue: 0.87))
        case .black:
            return CIImage(color: CIColor(red: 0.02, green: 0.02, blue: 0.03)).cropped(to: extent)
        case .green: // broadcast green: key it out in OBS with one checkbox
            return CIImage(color: CIColor(red: 0.0, green: 0.69, blue: 0.25)).cropped(to: extent)
        }
    }
}
