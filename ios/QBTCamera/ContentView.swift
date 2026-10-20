import AVFoundation
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var engine: StreamEngine
    @State private var showPreview = true
    @State private var dimmed = false
    @State private var savedBrightness: CGFloat = 0.5

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            ScrollView {
                VStack(spacing: 16) {
                    header
                    if showPreview {
                        CameraPreview(session: engine.camera.session)
                            .frame(height: 220)
                            .clipShape(RoundedRectangle(cornerRadius: 16))
                    }
                    statusCard
                    settingsCard
                    Button {
                        setDimmed(true)
                    } label: {
                        Label("Затемнить экран", systemImage: "moon.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.large)
                }
                .padding()
            }
            if dimmed {
                Color.black
                    .ignoresSafeArea()
                    .overlay(alignment: .bottom) {
                        Text("Трансляция идёт · коснитесь, чтобы включить экран")
                            .font(.footnote)
                            .foregroundStyle(.white.opacity(0.25))
                            .padding(.bottom, 40)
                    }
                    .onTapGesture { setDimmed(false) }
            }
        }
        .preferredColorScheme(.dark)
        .statusBarHidden(dimmed)
    }

    private var header: some View {
        HStack {
            Image(systemName: "camera.aperture")
                .font(.title)
                .foregroundStyle(.tint)
            Text("QBT Camera")
                .font(.title2.bold())
            Spacer()
        }
    }

    private var statusCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Circle()
                    .fill(engine.clientName == nil ? Color.orange : Color.green)
                    .frame(width: 10, height: 10)
                Text(engine.clientName.map { "Подключено: \($0)" } ?? "Ожидание компьютера…")
                    .font(.headline)
            }
            if let error = engine.errorMessage {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(.red)
            }
            infoRow("Wi-Fi", engine.wifiAddress.map { "\($0) : \(QBTProtocol.videoPort)" } ?? "не подключён")
            infoRow("USB", "подключите кабель к компьютеру")
            if engine.clientName != nil {
                infoRow("Поток", String(format: "%d к/с · %.1f Мбит/с", engine.fps, engine.megabitsPerSecond))
                if let roundTrip = engine.roundTripMillis {
                    infoRow("Задержка сети", String(format: "%.0f мс", roundTrip))
                }
                if engine.targetBitrate < engine.preset.bitrate {
                    infoRow("Качество снижено под сеть",
                            "\(engine.targetBitrate / 1_000_000) из \(engine.preset.bitrate / 1_000_000) Мбит/с")
                }
                if engine.droppedFrames > 0 {
                    infoRow("Пропущено кадров", "\(engine.droppedFrames)")
                }
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 16))
    }

    private var settingsCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Picker("Камера", selection: $engine.lens) {
                ForEach(CameraLens.available) { Text($0.title).tag($0) }
            }
            .pickerStyle(.segmented)

            Picker("Разрешение", selection: $engine.preset.resolution) {
                ForEach(StreamResolution.allCases) { Text($0.title).tag($0) }
            }
            .pickerStyle(.segmented)

            Picker("Кадров в секунду", selection: $engine.preset.fps) {
                ForEach(StreamPreset.frameRates, id: \.self) { Text("\($0) к/с").tag($0) }
            }
            .pickerStyle(.segmented)

            if engine.preset.resolution == .uhd4K {
                Text("4K лучше передавать по USB-кабелю: по Wi-Fi нужно \(engine.preset.bitrate / 1_000_000) Мбит/с. Телефон в 4K греется сильнее.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            HStack {
                Text("Ориентация")
                Spacer()
                Picker("Ориентация", selection: $engine.orientation) {
                    ForEach(VideoOrientation.allCases) { Text($0.title).tag($0) }
                }
            }

            Toggle("Предпросмотр на телефоне", isOn: $showPreview)
        }
        .padding()
        .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 16))
    }

    private func infoRow(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title).foregroundStyle(.secondary)
            Spacer()
            Text(value).monospacedDigit()
        }
        .font(.subheadline)
    }

    /// Screen is the biggest power draw after the camera: black it out during long calls.
    private func setDimmed(_ value: Bool) {
        if value {
            savedBrightness = UIScreen.main.brightness
            UIScreen.main.brightness = 0
        } else {
            UIScreen.main.brightness = savedBrightness
        }
        dimmed = value
    }
}

struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspect
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {}

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }
}
