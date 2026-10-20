#pragma once
#include <mfvirtualcamera.h>

#include "Common.h"

// Registers "QBT Camera" with Windows (Windows 11 virtual camera API).
// Needs QBTVirtualCamera.dll installed with windows/install_camera.ps1.
//
// Two ways to exist:
//  - system camera: registered once by the installer for all users; it stays in every app's
//    camera list even when QBTCamera.exe is closed (apps then see the "open QBT Camera" picture);
//  - session camera: fallback without the installer step, alive only while this app runs.
class VirtualCamera {
public:
    ~VirtualCamera() { Stop(); }

    /// Session camera, unless the system camera is installed (then nothing to do).
    HRESULT Start();
    void Stop();

    /// Run elevated (QBTCamera.exe --install-camera / --uninstall-camera).
    static HRESULT InstallSystemCamera();
    static HRESULT UninstallSystemCamera();
    static bool IsSystemCameraInstalled();

private:
    ComPtr<IMFVirtualCamera> camera_;
};
