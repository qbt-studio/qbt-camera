import Foundation

/// Microphone level for the VU bars. Fed from the audio queue, published ~12 times a second
/// in its own object so the level animation doesn't redraw the whole screen.
final class AudioMeter: ObservableObject {
    @Published private(set) var level: Float = 0

    private let lock = NSLock()
    private var peak: Float = 0
    private var timer: Timer?

    /// Called on the audio queue with 16-bit PCM.
    func record(_ pcm: Data) {
        var loudest: Int32 = 0
        pcm.withUnsafeBytes { raw in
            for sample in raw.bindMemory(to: Int16.self) { loudest = max(loudest, abs(Int32(sample))) }
        }
        let value = Float(loudest) / 32768
        lock.withLock { peak = max(peak, value) }
    }

    func start() {
        guard timer == nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 0.08, repeats: true) { [weak self] _ in
            guard let self else { return }
            let latest = self.lock.withLock { () -> Float in
                defer { self.peak = 0 }
                return self.peak
            }
            // Fast attack, slow release, like a hardware meter.
            let next = latest > self.level ? latest : self.level * 0.8
            if abs(next - self.level) > 0.005 { self.level = next }
        }
    }
}
