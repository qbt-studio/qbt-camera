import CoreGraphics

enum StreamResolution: String, CaseIterable, Identifiable {
    case hd720
    case fullHD
    case uhd4K

    var id: String { rawValue }

    var title: String {
        switch self {
        case .hd720: "720p"
        case .fullHD: "1080p"
        case .uhd4K: "4K"
        }
    }

    /// Landscape sensor dimensions; rotation to portrait happens on the capture connection.
    var dimensions: (width: Int32, height: Int32) {
        switch self {
        case .hd720: (1280, 720)
        case .fullHD: (1920, 1080)
        case .uhd4K: (3840, 2160)
        }
    }
}

struct StreamPreset: Equatable {
    var resolution: StreamResolution
    var fps: Int

    static let frameRates = [30, 60]

    var dimensions: (width: Int32, height: Int32) { resolution.dimensions }

    /// H.264 bitrates that keep fine detail without overloading Wi-Fi; USB has plenty of headroom.
    var bitrate: Int {
        switch (resolution, fps >= 60) {
        case (.hd720, false): 6_000_000
        case (.hd720, true): 9_000_000
        case (.fullHD, false): 10_000_000
        case (.fullHD, true): 16_000_000
        case (.uhd4K, false): 35_000_000
        case (.uhd4K, true): 50_000_000
        }
    }
}

enum VideoOrientation: String, CaseIterable, Identifiable {
    case auto
    case landscape
    case landscapeFlipped
    case portrait

    var id: String { rawValue }

    var title: String {
        switch self {
        case .auto: "Авто"
        case .landscape: "Горизонтально"
        case .landscapeFlipped: "Горизонтально, перевёрнуто"
        case .portrait: "Вертикально"
        }
    }

    /// Fixed rotation for the capture connection, or nil to follow the phone's physical orientation.
    var fixedAngle: CGFloat? {
        switch self {
        case .auto: nil
        case .landscape: 0
        case .landscapeFlipped: 180
        case .portrait: 90
        }
    }
}
