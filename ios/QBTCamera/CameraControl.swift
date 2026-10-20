import AVFoundation

/// Applies commands from the PC to the active camera and describes the camera as JSON
/// (capabilities after every reconfiguration, live state a few times a second).
///
/// Commands ({"cmd": name, ...}):
///   lens {value: "ultrawide"|"wide"|"tele"|"front"}     (handled by StreamEngine)
///   exposureAuto · exposureManual {iso, shutterUs} · exposureBias {value: EV}
///   wbAuto · wbManual {kelvin, tint}
///   focusAuto · focusManual {lensPosition: 0…1}
///   zoom {value} · torch {value: 0…1, 0 = off}
enum CameraControl {
    /// Zoom beyond this is too blurry to be useful for a webcam.
    static let zoomLimit: CGFloat = 15

    /// Called on the capture session queue with the device locked for configuration.
    static func apply(_ command: [String: Any], to device: AVCaptureDevice) {
        guard let name = command["cmd"] as? String else { return }
        func number(_ key: String) -> Double? { (command[key] as? NSNumber)?.doubleValue }

        switch name {
        case "exposureAuto":
            if device.isExposureModeSupported(.continuousAutoExposure) { device.exposureMode = .continuousAutoExposure }
        case "exposureManual":
            guard device.isExposureModeSupported(.custom) else { return }
            let format = device.activeFormat
            let iso = Float(number("iso") ?? Double(device.iso)).clamped(format.minISO, format.maxISO)
            let micros = number("shutterUs") ?? device.exposureDuration.seconds * 1_000_000
            let requested = CMTime(value: CMTimeValue(micros.isFinite ? micros : 10_000), timescale: 1_000_000)
            let duration = CMTimeClampToRange(requested, range: CMTimeRange(start: format.minExposureDuration,
                                                                             end: longestExposure(of: device)))
            device.setExposureModeCustom(duration: duration, iso: iso)
        case "exposureBias":
            guard let value = number("value") else { return }
            device.setExposureTargetBias(Float(value).clamped(device.minExposureTargetBias, device.maxExposureTargetBias))
        case "wbAuto":
            if device.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) {
                device.whiteBalanceMode = .continuousAutoWhiteBalance
            }
        case "wbManual":
            guard device.isLockingWhiteBalanceWithCustomDeviceGainsSupported else { return }
            let values = AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(
                temperature: Float(number("kelvin") ?? 5000), tint: Float(number("tint") ?? 0))
            var gains = device.deviceWhiteBalanceGains(for: values)
            let maxGain = device.maxWhiteBalanceGain
            gains.redGain = gains.redGain.clamped(1, maxGain)
            gains.greenGain = gains.greenGain.clamped(1, maxGain)
            gains.blueGain = gains.blueGain.clamped(1, maxGain)
            device.setWhiteBalanceModeLocked(with: gains)
        case "focusAuto":
            if device.isFocusModeSupported(.continuousAutoFocus) { device.focusMode = .continuousAutoFocus }
        case "focusManual":
            guard device.isLockingFocusWithCustomLensPositionSupported, let position = number("lensPosition") else { return }
            device.setFocusModeLocked(lensPosition: Float(position).clamped(0, 1))
        case "zoom":
            guard let value = number("value") else { return }
            device.videoZoomFactor = CGFloat(value).clamped(device.minAvailableVideoZoomFactor, maxZoom(of: device))
        case "torch":
            guard device.hasTorch, let level = number("value") else { return }
            if level <= 0.01 {
                device.torchMode = .off
            } else if device.isTorchAvailable {
                try? device.setTorchModeOn(level: Float(level).clamped(0.01, AVCaptureDevice.maxAvailableTorchLevel))
            }
        default:
            break
        }
    }

    static func capabilities(of device: AVCaptureDevice, lens: CameraLens) -> [String: Any] {
        let format = device.activeFormat
        return [
            "evt": "caps",
            "lenses": CameraLens.available.map(\.rawValue).joined(separator: ","),
            "lens": lens.rawValue,
            "manualExposure": device.isExposureModeSupported(.custom),
            "minISO": finite(format.minISO),
            "maxISO": finite(format.maxISO),
            "minShutterUs": micros(format.minExposureDuration),
            "maxShutterUs": micros(longestExposure(of: device)),
            "minBias": finite(device.minExposureTargetBias),
            "maxBias": finite(device.maxExposureTargetBias),
            "manualWB": device.isLockingWhiteBalanceWithCustomDeviceGainsSupported,
            "manualFocus": device.isLockingFocusWithCustomLensPositionSupported,
            "minZoom": finite(device.minAvailableVideoZoomFactor),
            "maxZoom": finite(maxZoom(of: device)),
            "torch": device.hasTorch,
        ]
    }

    static func state(of device: AVCaptureDevice, lens: CameraLens) -> [String: Any] {
        // Gains outside 1…max (camera interrupted, torch switching) make AVFoundation throw: clamp first.
        var gains = device.deviceWhiteBalanceGains
        let maxGain = device.maxWhiteBalanceGain
        gains.redGain = gains.redGain.isFinite ? gains.redGain.clamped(1, maxGain) : 1
        gains.greenGain = gains.greenGain.isFinite ? gains.greenGain.clamped(1, maxGain) : 1
        gains.blueGain = gains.blueGain.isFinite ? gains.blueGain.clamped(1, maxGain) : 1
        let whiteBalance = device.temperatureAndTintValues(for: gains)
        return [
            "evt": "state",
            "lens": lens.rawValue,
            "exposureAuto": device.exposureMode != .custom,
            "iso": finite(device.iso),
            "shutterUs": micros(device.exposureDuration),
            "bias": finite(device.exposureTargetBias),
            "wbAuto": device.whiteBalanceMode != .locked,
            "kelvin": finite(whiteBalance.temperature),
            "tint": finite(whiteBalance.tint),
            "focusAuto": device.focusMode != .locked,
            "lensPosition": finite(device.lensPosition),
            "zoom": finite(device.videoZoomFactor),
            "torch": device.torchMode == .on ? finite(device.torchLevel) : 0,
            // iOS blocks the torch while the phone is hot (like "iPhone needs to cool down" in Camera).
            "torchAvailable": device.hasTorch && device.isTorchAvailable,
            "thermal": thermalState,
        ]
    }

    static func encode(_ object: [String: Any]) -> Data? {
        try? JSONSerialization.data(withJSONObject: object)
    }

    /// Exposure can't be longer than a frame without dropping the frame rate.
    private static func longestExposure(of device: AVCaptureDevice) -> CMTime {
        let frame = device.activeVideoMaxFrameDuration
        return frame.isValid ? CMTimeMinimum(device.activeFormat.maxExposureDuration, frame)
                             : device.activeFormat.maxExposureDuration
    }

    private static func maxZoom(of device: AVCaptureDevice) -> CGFloat {
        min(device.maxAvailableVideoZoomFactor, zoomLimit)
    }

    private static func micros(_ time: CMTime) -> Double {
        finite(time.seconds * 1_000_000)
    }

    /// JSONSerialization throws an Objective-C exception on NaN/infinity: never send them.
    private static func finite<T: BinaryFloatingPoint>(_ value: T) -> Double {
        let double = Double(value)
        return double.isFinite ? double : 0
    }

    private static var thermalState: String {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: "nominal"
        case .fair: "fair"
        case .serious: "serious"
        case .critical: "critical"
        @unknown default: "unknown"
        }
    }
}

extension Comparable {
    func clamped(_ lower: Self, _ upper: Self) -> Self {
        min(max(self, lower), upper)
    }
}
