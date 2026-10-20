#pragma once
#include "Common.h"
#include "../common/SharedFrame.h"

// Host side of the shared frame buffer (see common/SharedFrame.h). The camera DLL creates it
// when an app opens "QBT Camera"; until then frames simply aren't published.
class SharedFrameWriter {
public:
    SharedFrameWriter() = default;
    SharedFrameWriter(const SharedFrameWriter&) = delete;
    SharedFrameWriter& operator=(const SharedFrameWriter&) = delete;
    ~SharedFrameWriter();

    /// True while some app is pulling frames from the virtual camera.
    bool CameraInUse();
    /// Size the app asked for (defaults to 1920x1080).
    void RequestedSize(UINT& width, UINT& height);
    void Write(const uint8_t* nv12, UINT rowPitch, UINT width, UINT height);
    /// No frames are coming because the phone paused its camera: the camera shows a pause card meanwhile.
    void KeepAlivePaused();

private:
    bool EnsureOpen();

    HANDLE mapping_ = nullptr;
    HANDLE frameReady_ = nullptr;
    qbt::SharedFrameHeader* header_ = nullptr;
    ULONGLONG lastOpenAttempt_ = 0;
};
