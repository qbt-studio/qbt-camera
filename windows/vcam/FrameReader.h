#pragma once
#include "Pch.h"
#include "../common/SharedFrame.h"

// Camera side of the shared frame buffer (see common/SharedFrame.h).
class FrameReader {
public:
    FrameReader() = default;
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;
    ~FrameReader();

    /// Tells the host which size to render; called when an app picks a format.
    void SetRequestedSize(UINT32 width, UINT32 height);
    /// Waits for the host to publish a new frame. Returns false on timeout.
    bool WaitForFrame(DWORD timeoutMs);
    /// Copies the newest frame if the host is alive and it matches the requested size.
    bool CopyLatest(BYTE* destination, LONG pitch, UINT32 width, UINT32 height);
    /// The host is running but the phone paused its camera (app in the background).
    bool HostPaused();

private:
    bool EnsureOpen();

    HANDLE mapping_ = nullptr;
    HANDLE frameReady_ = nullptr;
    qbt::SharedFrameHeader* header_ = nullptr;
    ULONGLONG lastOpenAttempt_ = 0;
};
