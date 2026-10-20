import ActivityKit
import SwiftUI
import WidgetKit

@main
struct QBTCameraWidgets: WidgetBundle {
    var body: some Widget {
        StreamLiveActivity()
    }
}

/// QBT Camera status in the Dynamic Island and on the Lock Screen.
struct StreamLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: StreamActivityAttributes.self) { context in
            LockScreenView(state: context.state)
                .activityBackgroundTint(Color(red: 0.02, green: 0.024, blue: 0.043).opacity(0.85))
                .activitySystemActionForegroundColor(.white)
        } dynamicIsland: { context in
            let state = context.state
            return DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    HStack(spacing: 6) {
                        QBTLogoMark(size: 26)
                        Text("QBT Camera").font(.system(size: 14, weight: .bold))
                    }
                    .padding(.leading, 4)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    StatusBadge(state: state)
                        .padding(.trailing, 4)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    HStack(spacing: 8) {
                        InfoPill(icon: state.connection == "USB" ? "cable.connector" : "wifi",
                                 text: state.connection.isEmpty ? "Нет ПК" : state.connection)
                        InfoPill(icon: "video.fill", text: state.quality)
                        InfoPill(icon: state.microphone ? "mic.fill" : "mic.slash.fill",
                                 text: state.microphone ? "Звук" : "Без звука")
                        if let latency = state.latencyMs, state.isLive {
                            InfoPill(icon: "bolt.fill", text: "\(latency) мс")
                        }
                    }
                    .padding(.top, 4)
                }
            } compactLeading: {
                QBTLogoMark(size: 20)
            } compactTrailing: {
                Text(state.shortStatus)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(state.tint)
            } minimal: {
                Circle().fill(state.tint).frame(width: 10, height: 10)
            }
            .keylineTint(state.tint)
        }
    }
}

private struct LockScreenView: View {
    let state: StreamActivityAttributes.ContentState

    var body: some View {
        HStack(spacing: 12) {
            QBTLogoMark(size: 40)
            VStack(alignment: .leading, spacing: 3) {
                Text("QBT Camera").font(.system(size: 15, weight: .bold)).foregroundStyle(.white)
                Text(state.detail).font(.system(size: 12)).foregroundStyle(.white.opacity(0.7)).lineLimit(1)
            }
            Spacer(minLength: 8)
            StatusBadge(state: state)
        }
        .padding(16)
    }
}

private struct StatusBadge: View {
    let state: StreamActivityAttributes.ContentState

    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(state.tint).frame(width: 8, height: 8)
            Text(state.headline).font(.system(size: 12, weight: .bold)).lineLimit(1)
        }
        .foregroundStyle(state.tint)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(state.tint.opacity(0.15), in: Capsule())
    }
}

private struct InfoPill: View {
    let icon: String
    let text: String

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: icon)
            Text(text).lineLimit(1)
        }
        .font(.system(size: 11, weight: .semibold))
        .foregroundStyle(.white.opacity(0.85))
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .background(.white.opacity(0.1), in: Capsule())
    }
}
