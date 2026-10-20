import ActivityKit
import SwiftUI

/// Live Activity shown in the Dynamic Island and on the Lock Screen while QBT Camera runs.
/// Shared by the app (starts and updates it) and the widget extension (draws it).
struct StreamActivityAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        /// "USB", "Wi-Fi", or empty while waiting for the PC.
        var connection: String
        /// "1080p 30".
        var quality: String
        /// App in the background: iOS stopped the camera, the picture is paused, sound continues.
        var paused: Bool
        var microphone: Bool
        var latencyMs: Int?

        var isLive: Bool { !connection.isEmpty && !paused }

        var headline: String {
            if paused { return "Картинка на паузе" }
            return connection.isEmpty ? "Ожидание ПК" : "В эфире"
        }

        var detail: String {
            if paused { return microphone ? "Звук идёт · вернитесь в приложение" : "Вернитесь в приложение" }
            if connection.isEmpty { return "Откройте QBT Camera на компьютере" }
            let latency = latencyMs.map { " · \($0) мс" } ?? ""
            return "\(connection) · \(quality)\(latency)"
        }

        var shortStatus: String {
            if paused { return "Пауза" }
            return connection.isEmpty ? "Ждёт" : connection
        }

        var tint: Color {
            if paused { return Color(red: 1.0, green: 0.69, blue: 0.13) }
            return connection.isEmpty ? Color(red: 1.0, green: 0.69, blue: 0.13) : Color(red: 0.2, green: 0.83, blue: 0.6)
        }
    }
}

/// The QBT logo for places without the app's theme (widget extension).
struct QBTLogoMark: View {
    var size: CGFloat = 24

    var body: some View {
        Canvas { context, canvas in
            let unit = canvas.width / 100
            let outline = Path(roundedRect: CGRect(x: 11 * unit, y: 11 * unit, width: 72 * unit, height: 72 * unit),
                               cornerRadius: 24 * unit, style: .continuous)
            context.stroke(outline, with: .color(Color(red: 0.169, green: 0.498, blue: 1.0)), lineWidth: 10.5 * unit)
            let badge = Path(roundedRect: CGRect(x: 66 * unit, y: 66 * unit, width: 28 * unit, height: 28 * unit),
                             cornerRadius: 9 * unit, style: .continuous)
            context.fill(badge, with: .color(Color(red: 0.325, green: 0.608, blue: 1.0)))
        }
        .frame(width: size, height: size)
    }
}
