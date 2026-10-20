import Foundation

/// Wire format shared with the Windows side (windows/tools/qbt_viewer.py and, later, the host app).
/// Every packet: 20-byte little-endian header followed by `payloadLength` bytes.
///
///     offset size field
///     0      4    magic   'QBTV'
///     4      1    type    PacketType
///     5      1    flags   Flags
///     6      2    reserved
///     8      4    payloadLength
///     12     8    ptsMicros (capture time, iPhone host clock)
///
/// Video payload is one H.264 access unit in Annex B form; keyframes carry SPS/PPS in front.
///
/// Message payload (type 2) is a UTF-8 JSON object: camera capabilities ("evt":"caps") or
/// live state ("evt":"state"), see CameraControl.
/// Audio payload (type 3) is 48 kHz mono signed 16-bit little-endian PCM from the microphone.
///
/// PC → phone, each message starts with 8 bytes: UInt32 magic + UInt32 value.
///   'QBTK' ack:     value = total video frames received on this connection (wrapping);
///                   the phone uses it to cap frames in flight.
///   'QBTC' command: value = length of the JSON command that follows ({"cmd": ...}).
enum QBTProtocol {
    static let videoPort: UInt16 = 50500
    static let bonjourType = "_qbtcam._tcp"
    static let magic: UInt32 = 0x5654_4251 // "QBTV"
    static let ackMagic: UInt32 = 0x4B54_4251 // "QBTK"
    static let commandMagic: UInt32 = 0x4354_4251 // "QBTC"
    static let headerSize = 20
    static let ackSize = 8
    static let maxCommandSize = 64 * 1024

    enum PacketType: UInt8 {
        case video = 1
        case message = 2
        case audio = 3
    }

    struct Flags: OptionSet {
        let rawValue: UInt8
        static let keyframe = Flags(rawValue: 1 << 0)
    }

    static func makeHeader(type: PacketType, flags: Flags, payloadLength: Int, ptsMicros: UInt64) -> Data {
        var data = Data(capacity: headerSize + payloadLength)
        func put<T: FixedWidthInteger>(_ value: T) {
            withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
        }
        put(magic)
        put(type.rawValue)
        put(flags.rawValue)
        put(UInt16(0))
        put(UInt32(payloadLength))
        put(ptsMicros)
        return data
    }
}
