#include "FrameReader.h"

#include <sddl.h>

namespace {

// Full access for SYSTEM, LocalService (Frame Server) and admins; read/write for logged-on users
// so QBTCamera.exe can publish frames. Low integrity label: readable from any process level.
constexpr wchar_t kSharedObjectsSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;AU)(A;;GRGW;;;IU)S:(ML;;NW;;;LW)";

} // namespace

FrameReader::~FrameReader() {
    if (header_) UnmapViewOfFile(header_);
    if (mapping_) CloseHandle(mapping_);
    if (frameReady_) CloseHandle(frameReady_);
}

bool FrameReader::EnsureOpen() {
    if (header_) return true;
    const ULONGLONG now = GetTickCount64();
    if (now - lastOpenAttempt_ < 1000) return false;
    lastOpenAttempt_ = now;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSharedObjectsSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        qbt::Log("vcam: security descriptor failed (%lu)", GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};

    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE,
                                  static_cast<DWORD>(uint64_t(qbt::kSharedBytes) >> 32),
                                  static_cast<DWORD>(qbt::kSharedBytes), qbt::kSharedFrameName);
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    if (!frameReady_) frameReady_ = CreateEventW(&attributes, FALSE, FALSE, qbt::kFrameReadyEventName);
    LocalFree(descriptor);

    if (!mapping_) {
        qbt::Log("vcam: CreateFileMapping failed (%lu)", GetLastError());
        return false;
    }
    header_ = static_cast<qbt::SharedFrameHeader*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, qbt::kSharedBytes));
    if (!header_) {
        qbt::Log("vcam: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    if (!existed || header_->magic != qbt::kSharedMagic || header_->version != qbt::kSharedVersion) {
        ZeroMemory(header_, sizeof(qbt::SharedFrameHeader));
        header_->magic = qbt::kSharedMagic;
        header_->version = qbt::kSharedVersion;
        header_->latestSlot = -1;
    }
    qbt::Log("vcam: shared frame buffer ready (existed=%d)", existed ? 1 : 0);
    return true;
}

void FrameReader::SetRequestedSize(UINT32 width, UINT32 height) {
    if (!EnsureOpen()) return;
    InterlockedExchange(&header_->requestedWidth, static_cast<LONG>(width));
    InterlockedExchange(&header_->requestedHeight, static_cast<LONG>(height));
    InterlockedExchange64(&header_->consumerHeartbeat, static_cast<LONG64>(GetTickCount64()));
}

bool FrameReader::WaitForFrame(DWORD timeoutMs) {
    if (!EnsureOpen() || !frameReady_) {
        Sleep(timeoutMs);
        return false;
    }
    return WaitForSingleObject(frameReady_, timeoutMs) == WAIT_OBJECT_0;
}

bool FrameReader::CopyLatest(BYTE* destination, LONG pitch, UINT32 width, UINT32 height) {
    if (!EnsureOpen()) return false;
    const ULONGLONG now = GetTickCount64();
    InterlockedExchange64(&header_->consumerHeartbeat, static_cast<LONG64>(now));
    if (now - static_cast<ULONGLONG>(header_->hostHeartbeat) > qbt::kHeartbeatTimeoutMs) return false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG slot = header_->latestSlot;
        if (slot < 0 || slot >= qbt::kSlotCount) return false;
        qbt::SharedFrameSlot& frame = header_->slots[slot];
        const LONG64 before = frame.sequence;
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        if (frame.width != width || frame.height != height) return false; // host hasn't switched size yet

        const BYTE* source = qbt::SlotData(header_, slot);
        for (UINT32 row = 0; row < height * 3 / 2; ++row) // Y plane, then interleaved UV plane
            memcpy(destination + size_t(row) * pitch, source + size_t(row) * width, width);
        MemoryBarrier();
        if (frame.sequence == before) return true;
    }
    return false;
}
