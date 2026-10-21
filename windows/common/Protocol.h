#pragma once
#include <cstdint>

// Wire format of the iPhone stream; mirrors ios/QBTCamera/Protocol.swift.
namespace qbt {

inline constexpr uint16_t kVideoPort = 50500;
inline constexpr uint32_t kPacketMagic = 0x56544251; // 'QBTV'
inline constexpr uint32_t kAckMagic = 0x4B544251;     // 'QBTK'
inline constexpr uint32_t kCommandMagic = 0x43544251; // 'QBTC': followed by `value` bytes of JSON
inline constexpr uint8_t kPacketTypeVideo = 1;
inline constexpr uint8_t kPacketTypeMessage = 2;      // JSON: {"evt":"caps"|"state", ...}
inline constexpr uint8_t kPacketTypeAudio = 3;        // 48 kHz mono int16 little-endian PCM
inline constexpr uint8_t kPacketTypeAuxVideo = 4;     // H.264 matte stream [foreground | blur], same pts as the picture
inline constexpr uint8_t kFlagKeyframe = 1;
inline constexpr uint32_t kMaxPacketBytes = 16 * 1024 * 1024;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t type;
    uint8_t flags;
    uint16_t reserved;
    uint32_t length;
    uint64_t ptsMicros; // capture time on the iPhone clock
};

// PC -> phone message header. Ack: value = video frames taken so far (the phone caps
// unacknowledged frames). Command: value = length of the JSON command that follows.
struct Ack {
    uint32_t magic;
    uint32_t framesReceived;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 20);
static_assert(sizeof(Ack) == 8);

} // namespace qbt
