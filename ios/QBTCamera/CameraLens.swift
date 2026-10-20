import AVFoundation

/// Physical cameras the phone UI and the PC can pick. Raw values are the wire names.
enum CameraLens: String, CaseIterable, Identifiable {
    case ultraWide = "ultrawide"
    case wide
    case telephoto = "tele"
    case front

    var id: String { rawValue }

    var title: String {
        switch self {
        case .ultraWide: "0.5×"
        case .wide: "1×"
        case .telephoto: Self.telephotoTitle
        case .front: "Фронт"
        }
    }

    /// "3×" on a 14 Pro, "5×" on a 15 Pro Max: derived from where the triple camera switches lenses.
    private static let telephotoTitle: String = {
        guard let triple = AVCaptureDevice.default(.builtInTripleCamera, for: .video, position: .back) else { return "Теле" }
        let switchOvers = triple.virtualDeviceSwitchOverVideoZoomFactors.map(\.doubleValue)
        guard switchOvers.count >= 2, let wide = switchOvers.first, wide > 0, let tele = switchOvers.last else { return "Теле" }
        let factor = tele / wide
        return factor.rounded() == factor ? "\(Int(factor))×" : String(format: "%.1f×", factor)
    }()

    var device: AVCaptureDevice? {
        switch self {
        case .ultraWide: AVCaptureDevice.default(.builtInUltraWideCamera, for: .video, position: .back)
        case .wide: AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
        case .telephoto: AVCaptureDevice.default(.builtInTelephotoCamera, for: .video, position: .back)
        case .front: AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front)
        }
    }

    /// Lenses this iPhone actually has.
    static let available: [CameraLens] = allCases.filter { $0.device != nil }
}
