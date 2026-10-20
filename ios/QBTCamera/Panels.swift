import SwiftUI

/// Slider card with a title, live value and optional lock. While the finger is on it the slider
/// shows its own value; afterwards it follows the camera again.
struct ValueSlider: View {
    let title: String
    let systemImage: String
    var tint: Color = Palette.accent
    /// Live value from the camera, in the slider's own units.
    let value: Double
    let range: ClosedRange<Double>
    let format: (Double) -> String
    var locked: Bool? = nil
    var onLockTap: (() -> Void)? = nil
    let onChange: (Double) -> Void

    @State private var editingValue: Double?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Image(systemName: systemImage).foregroundStyle(tint)
                Text(title).foregroundStyle(.white.opacity(0.8))
                Spacer(minLength: 4)
                Text(format(editingValue ?? value))
                    .font(.system(size: 12, weight: .bold, design: .rounded))
                    .monospacedDigit()
                    .foregroundStyle(.white)
                    .contentTransition(.numericText(value: editingValue ?? value))
                    .animation(.snappy(duration: 0.3), value: format(editingValue ?? value))
                if let locked {
                    Button { onLockTap?() } label: {
                        Image(systemName: locked ? "lock.fill" : "lock.open.fill")
                            .foregroundStyle(locked ? Palette.warning : .white.opacity(0.4))
                            .contentTransition(.symbolEffect(.replace))
                            .symbolEffect(.bounce, value: locked)
                            .frame(width: 22, height: 22)
                    }
                    .buttonStyle(PressableStyle())
                }
            }
            .font(.system(size: 11, weight: .bold))

            Slider(value: Binding(get: { editingValue ?? value.clamped(range.lowerBound, range.upperBound) },
                                  set: { newValue in
                                      editingValue = newValue
                                      onChange(newValue)
                                  }),
                   in: range,
                   onEditingChanged: { editing in
                       guard !editing else { return }
                       // Give the camera a moment to report the new value before following it again.
                       Task {
                           try? await Task.sleep(for: .milliseconds(700))
                           editingValue = nil
                       }
                   })
                .tint(tint)
        }
        .padding(10)
        .background(.black.opacity(0.28), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).stroke(.white.opacity(0.08)))
    }
}

// MARK: - Exposure

struct ExposurePanel: View {
    @EnvironmentObject private var engine: StreamEngine
    private var state: CameraUIState { engine.cameraState }

    /// 1/30 … 1/1000; 1/50 and 1/100 avoid flicker under 50 Hz lighting.
    private let shutterStops: [Double] = [33_333, 20_000, 16_667, 10_000, 8_333, 4_000, 2_000, 1_000]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                SectionCaption(title: "Экспозиция", systemImage: "camera.aperture")
                Spacer()
                GlassChip(title: state.exposureAuto ? "Авто" : "Фикс",
                          systemImage: state.exposureAuto ? "a.circle.fill" : "lock.fill",
                          active: !state.exposureAuto) { toggleExposureLock() }
                    .disabled(!state.manualExposure)
            }

            Group {
            if state.exposureAuto {
                ValueSlider(title: "Яркость", systemImage: "plusminus.circle.fill", value: state.bias,
                            range: state.minBias...state.maxBias, format: { String(format: "%+.1f EV", $0) }) {
                    engine.perform(["cmd": "exposureBias", "value": $0])
                }
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(availableStops, id: \.self) { stop in
                            GlassChip(title: "1/\(Int((1_000_000 / stop).rounded()))",
                                      active: abs(stop - state.shutterMicros) / stop < 0.08,
                                      font: .system(size: 12, weight: .bold, design: .monospaced)) {
                                engine.perform(["cmd": "exposureManual", "iso": state.iso, "shutterUs": stop])
                            }
                        }
                    }
                }
            }
            }
            .transition(.opacity.combined(with: .scale(scale: 0.97)))
            .animation(.spring(response: 0.35, dampingFraction: 0.85), value: state.exposureAuto)

            HStack(spacing: 8) {
                ValueSlider(title: "ISO", systemImage: "sun.max.fill", tint: .orange, value: isoPosition(state.iso),
                            range: 0...1, format: { "\(Int(isoAt($0)))" }) {
                    engine.perform(["cmd": "exposureManual", "iso": isoAt($0), "shutterUs": state.shutterMicros])
                }
                .disabled(!state.manualExposure)
                ValueSlider(title: "Свет", systemImage: "thermometer.medium", tint: .cyan, value: state.kelvin,
                            range: 2500...8000, format: { "\(Int($0)) K" }, locked: !state.whiteBalanceAuto,
                            onLockTap: toggleWhiteBalanceLock) {
                    engine.perform(["cmd": "wbManual", "kelvin": $0, "tint": state.tint])
                }
                .disabled(!state.manualWhiteBalance)
            }

            ValueSlider(title: state.focusAuto ? "Фокус · авто" : "Фокус", systemImage: "scope", tint: Palette.live,
                        value: state.lensPosition, range: 0...1, format: { "\(Int($0 * 100))%" },
                        locked: !state.focusAuto, onLockTap: toggleFocusLock) {
                engine.perform(["cmd": "focusManual", "lensPosition": $0])
            }
            .disabled(!state.manualFocus)
        }
    }

    private var availableStops: [Double] {
        shutterStops.filter { $0 >= state.minShutterMicros * 0.99 && $0 <= state.maxShutterMicros * 1.01 }
    }

    // ISO on a logarithmic scale: equal slider steps feel like equal brightness steps.
    private func isoAt(_ position: Double) -> Double {
        state.minISO * pow(state.maxISO / state.minISO, position)
    }

    private func isoPosition(_ iso: Double) -> Double {
        guard iso > state.minISO, state.maxISO > state.minISO else { return 0 }
        return (log(iso / state.minISO) / log(state.maxISO / state.minISO)).clamped(0, 1)
    }

    private func toggleExposureLock() {
        if state.exposureAuto {
            engine.perform(["cmd": "exposureManual", "iso": state.iso, "shutterUs": state.shutterMicros])
        } else {
            engine.perform(["cmd": "exposureAuto"])
        }
    }

    private func toggleWhiteBalanceLock() {
        if state.whiteBalanceAuto {
            engine.perform(["cmd": "wbManual", "kelvin": state.kelvin, "tint": state.tint])
        } else {
            engine.perform(["cmd": "wbAuto"])
        }
    }

    private func toggleFocusLock() {
        if state.focusAuto {
            engine.perform(["cmd": "focusManual", "lensPosition": state.lensPosition])
        } else {
            engine.perform(["cmd": "focusAuto"])
        }
    }
}

// MARK: - Color

struct ColorPanel: View {
    @EnvironmentObject private var engine: StreamEngine

    private let filters: [(ColorFilter, String)] = [
        (.none, "Нет"), (.cinema, "Кино"), (.warm, "Тёплый"), (.cool, "Холодный"),
        (.vivid, "Яркий"), (.soft, "Мягкий"), (.mono, "Ч/Б"), (.noir, "Нуар"),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionCaption(title: "Цвет", systemImage: "sparkles")
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(filters, id: \.0) { filter, title in
                        GlassChip(title: title, active: engine.effectSettings.filter == filter) {
                            var settings = engine.effectSettings
                            settings.filter = filter
                            engine.updateEffects(settings)
                        }
                    }
                }
            }
            ValueSlider(title: "Сила", systemImage: "dial.medium.fill", value: engine.effectSettings.filterAmount,
                        range: 0...1, format: { "\(Int($0 * 100))%" }) { amount in
                var settings = engine.effectSettings
                settings.filterAmount = amount
                engine.updateEffects(settings)
            }
            .disabled(engine.effectSettings.filter == .none)
            .opacity(engine.effectSettings.filter == .none ? 0.5 : 1)
        }
    }
}

// MARK: - Background

struct BackgroundPanel: View {
    @EnvironmentObject private var engine: StreamEngine

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                SectionCaption(title: "Фон и лицо", systemImage: "person.and.background.dotted")
                Spacer()
                GlassChip(title: "Слежение", systemImage: "person.crop.rectangle", active: engine.effectSettings.framing) {
                    var settings = engine.effectSettings
                    settings.framing.toggle()
                    engine.updateEffects(settings)
                }
            }
            ValueSlider(title: "Размытие фона", systemImage: "camera.filters", tint: .purple, value: engine.effectSettings.blur,
                        range: 0...1, format: { $0 < 0.01 ? "выкл" : "\(Int($0 * 100))%" }) { blur in
                var settings = engine.effectSettings
                settings.blur = blur
                engine.updateEffects(settings)
            }
            ValueSlider(title: "Ретушь кожи", systemImage: "face.smiling", tint: .pink, value: engine.effectSettings.retouch,
                        range: 0...1, format: { $0 < 0.01 ? "выкл" : "\(Int($0 * 100))%" }) { retouch in
                var settings = engine.effectSettings
                settings.retouch = retouch
                engine.updateEffects(settings)
            }
        }
    }
}

// MARK: - Stream

struct StreamPanel: View {
    @EnvironmentObject private var engine: StreamEngine

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionCaption(title: "Поток", systemImage: "bolt.fill")
            HStack(spacing: 6) {
                ForEach(StreamResolution.allCases) { resolution in
                    GlassChip(title: resolution.title, active: engine.preset.resolution == resolution) {
                        engine.preset.resolution = resolution
                    }
                }
                Spacer(minLength: 0)
                ForEach(StreamPreset.frameRates, id: \.self) { fps in
                    GlassChip(title: "\(fps) к/с", active: engine.preset.fps == fps) { engine.preset.fps = fps }
                }
            }

            HStack(spacing: 10) {
                GlassChip(title: "Микрофон", systemImage: engine.audioEnabled ? "mic.fill" : "mic.slash.fill",
                          active: engine.audioEnabled) { engine.audioEnabled.toggle() }
                AudioLevelBar(meter: engine.audioMeter)
            }
            if engine.microphoneDenied {
                Text("Нет доступа к микрофону: Настройки → QBT Camera → Микрофон.")
                    .font(.system(size: 11))
                    .foregroundStyle(Palette.danger)
            }

            VStack(spacing: 5) {
                infoRow("Подключение", engine.clientName.map { $0 == "USB" ? "USB-кабель" : "Wi-Fi" } ?? "ожидание компьютера")
                infoRow("Адрес в Wi-Fi", engine.wifiAddress.map { "\($0) : \(QBTProtocol.videoPort)" } ?? "нет Wi-Fi")
                if engine.clientName != nil {
                    infoRow("Поток", String(format: "%d к/с · %.1f Мбит/с", engine.fps, engine.megabitsPerSecond))
                    if let roundTrip = engine.roundTripMillis {
                        infoRow("Задержка сети", String(format: "%.0f мс", roundTrip))
                    }
                    if engine.targetBitrate < engine.preset.bitrate {
                        infoRow("Качество под сеть", "\(engine.targetBitrate / 1_000_000) из \(engine.preset.bitrate / 1_000_000) Мбит/с")
                    }
                }
                infoRow("Нагрев", engine.thermal.title, color: engine.thermal.color)
            }
            .padding(10)
            .background(.black.opacity(0.28), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        }
    }

    private func infoRow(_ title: String, _ value: String, color: Color = .white) -> some View {
        HStack {
            Text(title).foregroundStyle(Palette.secondaryText)
            Spacer()
            Text(value)
                .monospacedDigit()
                .foregroundStyle(color)
                .contentTransition(.numericText())
                .animation(.snappy, value: value)
        }
        .font(.system(size: 12, weight: .medium))
    }
}

struct AudioLevelBar: View {
    @ObservedObject var meter: AudioMeter

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(.white.opacity(0.12))
                Capsule()
                    .fill(LinearGradient(colors: [Palette.live, .cyan, meter.level > 0.8 ? Palette.warning : .purple],
                                         startPoint: .leading, endPoint: .trailing))
                    .frame(width: proxy.size.width * CGFloat(min(1, meter.level * 1.6)))
                    .animation(.easeOut(duration: 0.12), value: meter.level)
            }
        }
        .frame(height: 8)
    }
}

extension ProcessInfo.ThermalState {
    /// iOS reports a level, not degrees.
    var title: String {
        switch self {
        case .nominal: "в норме"
        case .fair: "тёплый"
        case .serious: "горячий"
        case .critical: "перегрев"
        @unknown default: "—"
        }
    }

    var color: Color {
        switch self {
        case .nominal: Palette.live
        case .fair: .yellow
        case .serious: Palette.warning
        case .critical: Palette.danger
        @unknown default: .white
        }
    }
}
