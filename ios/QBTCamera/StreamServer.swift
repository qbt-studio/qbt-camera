import Foundation
import Network

/// TCP server the PC connects to, over Wi-Fi or over USB (usbmux delivers USB connections to loopback).
/// One client at a time; the newest connection wins.
///
/// Latency control: the PC acknowledges every frame it has taken. Frames sent but not yet
/// acknowledged are "in flight"; beyond `frameBudget` we drop new frames instead of queueing them,
/// so video never piles up in socket buffers (which on Wi-Fi can hold seconds of 4K).
final class StreamServer {
    struct Stats {
        var bytes = 0
        var frames = 0
        var dropped = 0
        var roundTripTotal: UInt64 = 0
        var roundTripCount = 0

        /// Average send → ack time over the stats window, in milliseconds.
        var roundTripMillis: Double? {
            roundTripCount > 0 ? Double(roundTripTotal) / Double(roundTripCount) / 1_000_000 : nil
        }
    }

    /// Client description ("USB" or an IP address), nil when disconnected. Called on the server queue.
    var onClientChanged: ((String?) -> Void)?
    var onKeyframeNeeded: (() -> Void)?
    /// JSON command from the PC. Called on the server queue.
    var onCommand: ((Data) -> Void)?

    var hasClient: Bool { clientLock.withLock { clientConnected } }

    private let port: NWEndpoint.Port
    private let queue = DispatchQueue(label: "qbt.server", qos: .userInteractive)
    private var listener: NWListener?
    private var connection: NWConnection?
    private var stats = Stats()
    private let clientLock = NSLock()
    private var clientConnected = false

    // Flow control state, touched only on `queue`.
    private var frameBudget = 4
    private var framesSent: UInt32 = 0
    private var framesAcked: UInt32 = 0
    private var sendTimes = [UInt64](repeating: 0, count: 256)
    private var awaitingKeyframe = true
    private var keyframeWanted = false

    init(port: UInt16) {
        self.port = NWEndpoint.Port(rawValue: port)!
    }

    func start() {
        queue.async { self.startListener() }
    }

    /// About 100 ms of video may be in flight: enough to cover Wi-Fi round trips, too little to feel.
    func setFrameRate(_ fps: Int) {
        queue.async { self.frameBudget = max(3, fps / 10) }
    }

    func takeStats() -> Stats {
        queue.sync {
            defer { stats = Stats() }
            return stats
        }
    }

    /// Called on a VideoToolbox thread.
    func send(frame: Data, keyframe: Bool, ptsMicros: UInt64) {
        queue.async { [self] in
            guard let connection else { return }
            // After a drop, P-frames reference data the receiver never got: skip to the next keyframe.
            if awaitingKeyframe && !keyframe {
                stats.dropped += 1
                return
            }
            if framesInFlight >= frameBudget {
                stats.dropped += 1
                if !awaitingKeyframe {
                    awaitingKeyframe = true
                    keyframeWanted = true // requested once the link drains, see handleAck
                }
                return
            }
            awaitingKeyframe = false

            var packet = QBTProtocol.makeHeader(type: .video, flags: keyframe ? .keyframe : [],
                                                payloadLength: frame.count, ptsMicros: ptsMicros)
            packet.append(frame)
            sendTimes[Int(framesSent & 0xFF)] = DispatchTime.now().uptimeNanoseconds
            framesSent &+= 1
            stats.frames += 1
            stats.bytes += packet.count
            connection.send(content: packet, completion: .idempotent)
        }
    }

    /// Microphone PCM. Tiny compared to video (0.77 Mbit/s), so it bypasses the frame budget.
    func sendAudio(_ pcm: Data, ptsMicros: UInt64) {
        queue.async { [self] in
            guard let connection else { return }
            var packet = QBTProtocol.makeHeader(type: .audio, flags: [], payloadLength: pcm.count, ptsMicros: ptsMicros)
            packet.append(pcm)
            stats.bytes += packet.count
            connection.send(content: packet, completion: .idempotent)
        }
    }

    /// Matte side stream for PC-side background processing. Tiny (≈1 Mbit/s), sent right before the
    /// video frame with the same timestamp; not counted in the frame budget.
    func sendAux(frame: Data, keyframe: Bool, ptsMicros: UInt64) {
        queue.async { [self] in
            guard let connection else { return }
            var packet = QBTProtocol.makeHeader(type: .auxVideo, flags: keyframe ? [.keyframe] : [], payloadLength: frame.count,
                                                ptsMicros: ptsMicros)
            packet.append(frame)
            stats.bytes += packet.count
            connection.send(content: packet, completion: .idempotent)
        }
    }

    /// Small JSON message to the PC (capabilities, state). Not subject to the frame budget.
    func sendMessage(_ json: Data) {
        queue.async { [self] in
            guard let connection else { return }
            var packet = QBTProtocol.makeHeader(type: .message, flags: [], payloadLength: json.count, ptsMicros: 0)
            packet.append(json)
            connection.send(content: packet, completion: .idempotent)
        }
    }

    private var framesInFlight: Int { Int(framesSent &- framesAcked) }

    private func startListener() {
        guard listener == nil else { return }
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let parameters = NWParameters(tls: nil, tcp: tcp)
        parameters.allowLocalEndpointReuse = true
        do {
            let listener = try NWListener(using: parameters, on: port)
            listener.service = NWListener.Service(type: QBTProtocol.bonjourType)
            listener.newConnectionHandler = { [weak self] in self?.accept($0) }
            listener.stateUpdateHandler = { [weak self, weak listener] state in
                guard case .failed = state, let self else { return }
                listener?.cancel()
                self.listener = nil
                self.queue.asyncAfter(deadline: .now() + 1) { self.startListener() }
            }
            listener.start(queue: queue)
            self.listener = listener
        } catch {
            queue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.startListener() }
        }
    }

    private func accept(_ newConnection: NWConnection) {
        connection?.cancel()
        connection = newConnection
        framesSent = 0
        framesAcked = 0
        awaitingKeyframe = true
        keyframeWanted = false
        newConnection.stateUpdateHandler = { [weak self, weak newConnection] state in
            guard let self, let newConnection, self.connection === newConnection else { return }
            switch state {
            case .ready:
                self.setClient(Self.describe(newConnection.endpoint))
                self.onKeyframeNeeded?()
                self.receiveMessages(newConnection)
            case .failed, .cancelled:
                self.connection = nil
                self.setClient(nil)
            default:
                break
            }
        }
        newConnection.start(queue: queue)
    }

    /// Reads PC → phone messages: 8-byte acks and length-prefixed JSON commands.
    private func receiveMessages(_ connection: NWConnection) {
        connection.receive(minimumIncompleteLength: QBTProtocol.ackSize,
                           maximumLength: QBTProtocol.ackSize) { [weak self, weak connection] data, _, isComplete, error in
            guard let self, let connection else { return }
            if let data, data.count == QBTProtocol.ackSize, self.connection === connection {
                let magic = data.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self)) }
                let value = data.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: 4, as: UInt32.self)) }
                switch magic {
                case QBTProtocol.ackMagic:
                    self.handleAck(value)
                case QBTProtocol.commandMagic where value > 0 && value <= QBTProtocol.maxCommandSize:
                    self.receiveCommand(connection, length: Int(value))
                    return
                default:
                    connection.cancel()
                    return
                }
            }
            if isComplete || error != nil {
                connection.cancel()
            } else {
                self.receiveMessages(connection)
            }
        }
    }

    private func receiveCommand(_ connection: NWConnection, length: Int) {
        connection.receive(minimumIncompleteLength: length, maximumLength: length) { [weak self, weak connection] data, _, isComplete, error in
            guard let self, let connection else { return }
            if let data, data.count == length, self.connection === connection {
                self.onCommand?(data)
            }
            if isComplete || error != nil {
                connection.cancel()
            } else {
                self.receiveMessages(connection)
            }
        }
    }

    private func handleAck(_ count: UInt32) {
        let acknowledged = Int(count &- framesAcked)
        guard acknowledged > 0, acknowledged <= framesInFlight else { return }
        framesAcked = count
        let sentAt = sendTimes[Int((count &- 1) & 0xFF)]
        stats.roundTripTotal += DispatchTime.now().uptimeNanoseconds - sentAt
        stats.roundTripCount += 1
        // Ask for the recovery keyframe only now that the pipe is nearly empty, otherwise the
        // big keyframe itself would be dropped again and we'd loop requesting keyframes.
        if keyframeWanted && framesInFlight <= 1 {
            keyframeWanted = false
            onKeyframeNeeded?()
        }
    }

    private func setClient(_ description: String?) {
        clientLock.withLock { clientConnected = description != nil }
        onClientChanged?(description)
    }

    private static func describe(_ endpoint: NWEndpoint) -> String {
        guard case let .hostPort(host, _) = endpoint else { return "\(endpoint)" }
        let address = "\(host)".components(separatedBy: "%").first ?? "\(host)"
        return address.hasPrefix("127.") || address == "::1" ? "USB" : address
    }
}
