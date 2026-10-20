#pragma once
#include <mfvirtualcamera.h>

#include "Common.h"

// Registers "QBT Camera" with Windows for as long as this app runs (Windows 11 virtual camera API).
// Needs QBTVirtualCamera.dll installed once with windows/install_camera.ps1.
class VirtualCamera {
public:
    ~VirtualCamera() { Stop(); }
    HRESULT Start();
    void Stop();

private:
    ComPtr<IMFVirtualCamera> camera_;
};
