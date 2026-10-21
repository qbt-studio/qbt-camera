import SwiftUI

/// Black pill under the Dynamic Island: link, latency, microphone and quality at a glance.
struct StatusIsland: View {
    @EnvironmentObject private var engine: StreamEngine

    var body: some View {
        HStack(spacing: 10) {
            HStack(spacing: 6) {
                let connected = engine.clientName != nil
                LiveDot(color: connected ? Palette.live : Palette.warning, pulsing: connected)
                Text(engine.connectionLabel)
                    .font(.system(size: 11, weight: .bold))
                    .contentTransition(.opacity)
                if connected, let roundTrip = engine.roundTripMillis {
                    Text("\(Int(roundTrip)) ms")
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundStyle(Palette.live)
                        .contentTransition(.numericText(value: roundTrip))
                        .transition(.scale.combined(with: .opacity))
                }
            }
            .animation(.spring(response: 0.4, dampingFraction: 0.8), value: engine.clientName)
            .animation(.snappy, value: engine.roundTripMillis.map(Int.init))
            VUBars(meter: engine.audioMeter)
            Text("\(engine.preset.resolution.title) \(engine.preset.fps)")
                .font(.system(size: 10, weight: .heavy))
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(.white.opacity(0.1), in: RoundedRectangle(cornerRadius: 5))
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.black.opacity(0.92), in: Capsule())
        .overlay(Capsule().stroke(.white.opacity(0.14)))
        .frame(maxWidth: .infinity)
    }
}

/// Status dot with a soft radar pulse while live.
struct LiveDot: View {
    let color: Color
    let pulsing: Bool
    @State private var pulse = false

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 8, height: 8)
            .shadow(color: color, radius: 4)
            .background {
                if pulsing {
                    Circle()
                        .stroke(color, lineWidth: 1.5)
                        .scaleEffect(pulse ? 2.6 : 1)
                        .opacity(pulse ? 0 : 0.8)
                }
            }
            .onAppear {
                withAnimation(.easeOut(duration: 1.5).repeatForever(autoreverses: false)) { pulse = true }
            }
            .animation(.easeInOut(duration: 0.3), value: color)
    }
}

struct VUBars: View {
    @ObservedObject var meter: AudioMeter
    private let weights: [Float] = [0.5, 0.85, 1, 0.7, 0.4]

    var body: some View {
        HStack(alignment: .bottom, spacing: 2) {
            ForEach(weights.indices, id: \.self) { index in
                Capsule()
                    .fill(LinearGradient(colors: [Palette.live, .cyan, .purple], startPoint: .bottom, endPoint: .top))
                    .frame(width: 3, height: 3 + CGFloat(min(1, meter.level * 1.8 * weights[index])) * 9)
            }
        }
        .frame(height: 12, alignment: .bottom)
        .animation(.easeOut(duration: 0.1), value: meter.level)
    }
}

/// Rule-of-thirds guide.
struct GridOverlay: View {
    var body: some View {
        Canvas { context, size in
            var lines = Path()
            for fraction in [1.0 / 3, 2.0 / 3] {
                lines.move(to: CGPoint(x: size.width * fraction, y: 0))
                lines.addLine(to: CGPoint(x: size.width * fraction, y: size.height))
                lines.move(to: CGPoint(x: 0, y: size.height * fraction))
                lines.addLine(to: CGPoint(x: size.width, y: size.height * fraction))
            }
            context.stroke(lines, with: .color(.white.opacity(0.22)), lineWidth: 0.8)
        }
    }
}

// MARK: - Teleprompter

/// Script scrolling right under the front camera: read it while looking into the lens.
struct PrompterCard: View {
    @EnvironmentObject private var engine: StreamEngine
    @Binding var editing: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Label("Look here — into the lens", systemImage: "eye.fill")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(.cyan)
                    .lineLimit(1)
                Spacer(minLength: 4)
                GlassChip(title: "\(Int(engine.prompterSpeed))×", font: .system(size: 11, weight: .bold)) {
                    engine.prompterSpeed = engine.prompterSpeed >= 5 ? 1 : engine.prompterSpeed + 1
                }
                GlassIconButton(systemImage: engine.prompterPlaying ? "pause.fill" : "play.fill", size: 30) {
                    engine.prompterPlaying.toggle()
                }
                GlassIconButton(systemImage: "pencil", size: 30) { editing = true }
                GlassIconButton(systemImage: "xmark", size: 30) { engine.prompterVisible = false }
            }
            PrompterScroller(text: engine.prompterText, speed: engine.prompterSpeed, playing: engine.prompterPlaying)
                .frame(height: 112)
                .clipped()
                .mask(LinearGradient(stops: [.init(color: .clear, location: 0), .init(color: .black, location: 0.12),
                                             .init(color: .black, location: 0.82), .init(color: .clear, location: 1)],
                                     startPoint: .top, endPoint: .bottom))
        }
        .padding(12)
        .glass(in: RoundedRectangle(cornerRadius: 26, style: .continuous), tint: Color.cyan.opacity(0.12))
    }
}

struct PrompterScroller: View {
    let text: String
    let speed: Double
    let playing: Bool

    @State private var baseOffset: CGFloat = 0
    @State private var anchor = Date()
    @State private var textHeight: CGFloat = 1

    private func pointsPerSecond(_ speed: Double) -> CGFloat { CGFloat(speed) * 14 }

    var body: some View {
        TimelineView(.animation(paused: !playing)) { context in
            let elapsed = playing ? CGFloat(context.date.timeIntervalSince(anchor)) : 0
            let cycle = textHeight + 60
            let offset = (baseOffset + elapsed * pointsPerSecond(speed)).truncatingRemainder(dividingBy: cycle)
            ZStack(alignment: .topLeading) {
                Text(text)
                    .font(.system(size: 21, weight: .semibold))
                    .foregroundStyle(.white)
                    .lineSpacing(4)
                    .fixedSize(horizontal: false, vertical: true)
                    .background(GeometryReader { proxy in
                        Color.clear
                            .onAppear { textHeight = max(1, proxy.size.height) }
                            .onChange(of: proxy.size.height) { _, height in textHeight = max(1, height) }
                    })
                    .offset(y: 16 - offset)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        // Keep the position when pausing or changing speed.
        .onChange(of: playing) { wasPlaying, _ in
            if wasPlaying { baseOffset += CGFloat(Date().timeIntervalSince(anchor)) * pointsPerSecond(speed) }
            anchor = Date()
        }
        .onChange(of: speed) { oldSpeed, _ in
            if playing { baseOffset += CGFloat(Date().timeIntervalSince(anchor)) * pointsPerSecond(oldSpeed) }
            anchor = Date()
        }
        .onChange(of: text) { _, _ in
            baseOffset = 0
            anchor = Date()
        }
    }
}

struct PrompterEditor: View {
    @EnvironmentObject private var engine: StreamEngine
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            TextEditor(text: $engine.prompterText)
                .font(.system(size: 18))
                .scrollContentBackground(.hidden)
                .padding()
                .background(Palette.background)
                .navigationTitle("Teleprompter text")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                    }
                }
        }
        .preferredColorScheme(.dark)
    }
}

// MARK: - Blackout

/// Screen off while streaming continues: the display is the biggest heat source on long calls.
struct BlackoutView: View {
    @EnvironmentObject private var engine: StreamEngine
    let onWake: () -> Void

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 16) {
                QBTLogo(size: 52)
                    .phaseAnimator([false, true]) { logo, breathing in
                        logo.scaleEffect(breathing ? 1.06 : 0.97).opacity(breathing ? 1 : 0.8)
                    } animation: { _ in .easeInOut(duration: 1.8) }
                HStack(spacing: 6) {
                    let connected = engine.clientName != nil
                    LiveDot(color: connected ? Palette.live : Palette.warning, pulsing: connected)
                    Text(connected ? "STREAMING \(engine.preset.resolution.title) \(engine.preset.fps)" : "WAITING FOR PC")
                        .font(.system(size: 12, weight: .heavy))
                        .tracking(1.2)
                        .foregroundStyle(connected ? Palette.live : Palette.warning)
                }
                Text("The screen is off to keep the iPhone cool. The camera keeps running.")
                    .font(.system(size: 13, weight: .semibold))
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.white.opacity(0.8))
                HStack(spacing: 8) {
                    stat("LINK", engine.connectionLabel, .white)
                    stat("LATENCY", engine.roundTripMillis.map { "\(Int($0)) ms" } ?? "—", Palette.live)
                    stat("HEAT", engine.thermal.title, engine.thermal.color)
                }
                Text("Tap to wake the screen")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(.white.opacity(0.35))
            }
            .padding(24)
            .frame(maxWidth: 310)
            .background(.white.opacity(0.04), in: RoundedRectangle(cornerRadius: 36, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 36, style: .continuous).stroke(.white.opacity(0.1)))
        }
        .contentShape(Rectangle())
        // Any touch wakes the screen, not only a clean tap.
        .gesture(DragGesture(minimumDistance: 0).onEnded { _ in onWake() })
    }

    private func stat(_ title: String, _ value: String, _ color: Color) -> some View {
        VStack(spacing: 3) {
            Text(title).font(.system(size: 8, weight: .bold)).foregroundStyle(.white.opacity(0.4))
            Text(value).font(.system(size: 12, weight: .bold)).foregroundStyle(color).lineLimit(1).minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 8)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}
