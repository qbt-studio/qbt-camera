#pragma once
#include <windows.h>
#include <cstdint>

// Frame exchange between QBTCamera.exe (writer, user session) and QBTVirtualCamera.dll
// (reader, loaded by the Windows Camera Frame Server service in session 0).
// The DLL creates both objects: only the service has the privilege to create names in Global\.
//
// Triple buffer of NV12 frames. A slot's sequence is odd while the host writes it, so the
// reader can detect a torn copy and retry with the newest slot.
namespace qbt {

inline constexpr wchar_t kSharedFrameName[] = L"Global\\QBTCamera_SharedFrame";
inline constexpr wchar_t kFrameReadyEventName[] = L"Global\\QBTCamera_FrameReady";
inline constexpr uint32_t kSharedMagic = 0x53544251; // 'QBTS'
inline constexpr uint32_t kSharedVersion = 1;
inline constexpr uint32_t kMaxWidth = 3840;
inline constexpr uint32_t kMaxHeight = 2160;
inline constexpr size_t kSlotBytes = size_t(kMaxWidth) * kMaxHeight * 3 / 2;
inline constexpr int kSlotCount = 3;
// A side is considered alive if it touched its heartbeat within this many milliseconds.
inline constexpr ULONGLONG kHeartbeatTimeoutMs = 1500;

struct SharedFrameSlot {
    volatile LONG64 sequence;
    uint32_t width;  // NV12, stride == width
    uint32_t height;
};

struct SharedFrameHeader {
    uint32_t magic;
    uint32_t version;
    // Written by the camera DLL: the format the app picked, so the host renders straight into it.
    volatile LONG requestedWidth;
    volatile LONG requestedHeight;
    volatile LONG64 consumerHeartbeat; // GetTickCount64() when the DLL last pulled a frame
    // Written by the host.
    volatile LONG64 hostHeartbeat;     // GetTickCount64() when the host last wrote a frame
    volatile LONG latestSlot;          // newest complete slot, -1 if none yet
    SharedFrameSlot slots[kSlotCount];
    // Written by the host (appended later: zero, i.e. live, for older hosts).
    volatile LONG hostStatus;          // kHostLive or kHostPaused
};

inline constexpr LONG kHostLive = 0;
// The phone app is in the background: iOS stops its camera, sound keeps going.
inline constexpr LONG kHostPaused = 1;

inline constexpr size_t kSharedHeaderBytes = (sizeof(SharedFrameHeader) + 4095) & ~size_t(4095);
inline constexpr size_t kSharedBytes = kSharedHeaderBytes + kSlotBytes * kSlotCount;

inline uint8_t* SlotData(SharedFrameHeader* header, int slot) {
    return reinterpret_cast<uint8_t*>(header) + kSharedHeaderBytes + kSlotBytes * size_t(slot);
}

} // namespace qbt
