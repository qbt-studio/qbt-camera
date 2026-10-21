import ActivityKit
import Foundation

/// Version 0.1 showed a Live Activity in the Dynamic Island; the app no longer does (a minimized
/// QBT Camera is fully off). This ends one left over from that version. ActivityKit finds it by the
/// attributes type name, so the old type is kept here, unused otherwise.
struct StreamActivityAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        var connection: String
        var quality: String
        var paused: Bool
        var microphone: Bool
        var latencyMs: Int?
    }
}

enum LiveActivityCleanup {
    static func endLeftovers() {
        Task {
            for activity in Activity<StreamActivityAttributes>.activities {
                await activity.end(nil, dismissalPolicy: .immediate)
            }
        }
    }
}
