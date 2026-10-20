import ActivityKit
import Foundation

/// Keeps one Live Activity (Dynamic Island + Lock Screen) in sync with the stream.
/// It can only be started while the app is in the foreground; updates also work from the background.
@MainActor
final class LiveActivityController {
    private var activity: Activity<StreamActivityAttributes>?
    private var lastState: StreamActivityAttributes.ContentState?
    private var lastUpdate = Date.distantPast

    /// Ends activities left over from a previous run (the app may have been killed without ending them).
    func start(with state: StreamActivityAttributes.ContentState) {
        Task {
            for old in Activity<StreamActivityAttributes>.activities {
                await old.end(nil, dismissalPolicy: .immediate)
            }
            activity = nil
            update(state)
        }
    }

    func update(_ state: StreamActivityAttributes.ContentState) {
        guard ActivityAuthorizationInfo().areActivitiesEnabled, state != lastState else { return }
        // Latency alone changes every second: refresh it at most every 5 s to spare the system budget.
        if let lastState, onlyLatencyChanged(lastState, state), Date().timeIntervalSince(lastUpdate) < 5 { return }
        lastState = state
        lastUpdate = Date()
        let content = ActivityContent(state: state, staleDate: nil)
        if let activity {
            Task { await activity.update(content) }
        } else {
            activity = try? Activity.request(attributes: StreamActivityAttributes(), content: content, pushType: nil)
        }
    }

    private func onlyLatencyChanged(_ old: StreamActivityAttributes.ContentState,
                                    _ new: StreamActivityAttributes.ContentState) -> Bool {
        var withoutLatency = new
        withoutLatency.latencyMs = old.latencyMs
        return withoutLatency == old
    }
}
