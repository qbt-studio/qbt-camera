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
        case .telephoto: "Теле"
        case .front: "Фронт"
        }
    }

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
