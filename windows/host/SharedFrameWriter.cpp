#include "SharedFrameWriter.h"

SharedFrameWriter::~SharedFrameWriter() {
    if (header_) UnmapViewOfFile(header_);
    if (mapping_) CloseHandle(mapping_);
    if (frameReady_) CloseHandle(frameReady_);
}

bool SharedFrameWriter::EnsureOpen() {
    if (header_) return true;
    const ULONGLONG now = GetTickCount64();
    if (now - lastOpenAttempt_ < 500) return false;
    lastOpenAttempt_ = now;

    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, qbt::kSharedFrameName);
    if (!mapping_) return false;
    header_ = static_cast<qbt::SharedFrameHeader*>(
        MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, qbt::kSharedBytes));
    if (!header_ || header_->magic != qbt::kSharedMagic || header_->version != qbt::kSharedVersion) {
        qbt::Log("camera output: shared buffer unusable (%lu)", GetLastError());
        if (header_) UnmapViewOfFile(header_);
        header_ = nullptr;
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    frameReady_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, qbt::kFrameReadyEventName);
    qbt::Log("camera output: connected to the virtual camera");
    return true;
}

bool SharedFrameWriter::CameraInUse() {
    if (!EnsureOpen()) return false;
    return GetTickCount64() - static_cast<ULONGLONG>(header_->consumerHeartbeat) < qbt::kHeartbeatTimeoutMs;
}

void SharedFrameWriter::RequestedSize(UINT& width, UINT& height) {
    width = 1920;
    height = 1080;
    if (!EnsureOpen()) return;
    const LONG requestedWidth = header_->requestedWidth;
    const LONG requestedHeight = header_->requestedHeight;
    if (requestedWidth >= 64 && requestedHeight >= 64 && requestedWidth <= static_cast<LONG>(qbt::kMaxWidth) &&
        requestedHeight <= static_cast<LONG>(qbt::kMaxHeight)) {
        width = static_cast<UINT>(requestedWidth) & ~1u;
        height = static_cast<UINT>(requestedHeight) & ~1u;
    }
}

void SharedFrameWriter::Write(const uint8_t* nv12, UINT rowPitch, UINT width, UINT height) {
    if (!EnsureOpen() || width > qbt::kMaxWidth || height > qbt::kMaxHeight) return;
    const LONG latest = header_->latestSlot;
    const int slot = latest < 0 ? 0 : (latest + 1) % qbt::kSlotCount;
    qbt::SharedFrameSlot& frame = header_->slots[slot];

    InterlockedIncrement64(&frame.sequence); // odd: being written
    frame.width = width;
    frame.height = height;
    uint8_t* destination = qbt::SlotData(header_, slot);
    for (UINT row = 0; row < height * 3 / 2; ++row) // Y rows, then interleaved UV rows
        memcpy(destination + size_t(row) * width, nv12 + size_t(row) * rowPitch, width);
    InterlockedIncrement64(&frame.sequence); // even: complete

    InterlockedExchange(&header_->latestSlot, slot);
    InterlockedExchange(&header_->hostStatus, qbt::kHostLive);
    InterlockedExchange64(&header_->hostHeartbeat, static_cast<LONG64>(GetTickCount64()));
    if (frameReady_) SetEvent(frameReady_);
}

void SharedFrameWriter::KeepAlivePaused() {
    if (!EnsureOpen()) return;
    InterlockedExchange(&header_->hostStatus, qbt::kHostPaused);
    InterlockedExchange64(&header_->hostHeartbeat, static_cast<LONG64>(GetTickCount64()));
}
