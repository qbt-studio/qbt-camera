import Foundation

/// Live camera values and limits for the phone's controls (the same data the PC gets as JSON).
struct CameraUIState: Equatable {
    var exposureAuto = true
    var iso = 100.0
    var shutterMicros = 16_667.0
    var bias = 0.0
    var whiteBalanceAuto = true
    var kelvin = 5000.0
    var tint = 0.0
    var focusAuto = true
    var lensPosition = 0.5
    var zoom = 1.0
    var torch = 0.0
    var torchAvailable = true

    var minISO = 50.0
    var maxISO = 3200.0
    var minShutterMicros = 100.0
    var maxShutterMicros = 33_333.0
    var minBias = -2.0
    var maxBias = 2.0
    var minZoom = 1.0
    var maxZoom = 10.0
    var hasTorch = false
    var manualExposure = true
    var manualWhiteBalance = true
    var manualFocus = true

    init() {}

    init(state: [String: Any], caps: [String: Any]) {
        func number(_ values: [String: Any], _ key: String, _ fallback: Double) -> Double {
            (values[key] as? Double) ?? (values[key] as? NSNumber)?.doubleValue ?? fallback
        }
        func flag(_ values: [String: Any], _ key: String, _ fallback: Bool) -> Bool {
            (values[key] as? Bool) ?? fallback
        }
        exposureAuto = flag(state, "exposureAuto", true)
        iso = number(state, "iso", iso).rounded()
        shutterMicros = number(state, "shutterUs", shutterMicros).rounded()
        bias = (number(state, "bias", 0) * 20).rounded() / 20
        whiteBalanceAuto = flag(state, "wbAuto", true)
        kelvin = (number(state, "kelvin", kelvin) / 10).rounded() * 10
        tint = number(state, "tint", 0).rounded()
        focusAuto = flag(state, "focusAuto", true)
        lensPosition = (number(state, "lensPosition", lensPosition) * 100).rounded() / 100
        zoom = (number(state, "zoom", 1) * 100).rounded() / 100
        torch = (number(state, "torch", 0) * 100).rounded() / 100
        torchAvailable = flag(state, "torchAvailable", true)

        minISO = max(1, number(caps, "minISO", minISO))
        maxISO = max(minISO + 1, number(caps, "maxISO", maxISO))
        minShutterMicros = number(caps, "minShutterUs", minShutterMicros)
        maxShutterMicros = number(caps, "maxShutterUs", maxShutterMicros)
        minBias = max(-3, number(caps, "minBias", minBias))
        maxBias = min(3, number(caps, "maxBias", maxBias))
        minZoom = number(caps, "minZoom", 1)
        maxZoom = max(minZoom + 0.1, number(caps, "maxZoom", maxZoom))
        hasTorch = flag(caps, "torch", false)
        manualExposure = flag(caps, "manualExposure", true)
        manualWhiteBalance = flag(caps, "manualWB", true)
        manualFocus = flag(caps, "manualFocus", true)
    }
}
