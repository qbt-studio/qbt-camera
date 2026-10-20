#include "VirtualCamera.h"

#include "../common/CameraIds.h"

HRESULT VirtualCamera::Start() {
    if (camera_) return S_OK;
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                                       MFVirtualCameraAccess_CurrentUser, qbt::kVirtualCameraName,
                                       qbt::kVirtualCameraClsid, nullptr, 0, &camera_);
    if (SUCCEEDED(hr)) hr = camera_->Start(nullptr);
    if (FAILED(hr)) {
        qbt::Log("virtual camera: start failed 0x%08lx", hr);
        camera_.Reset();
        return hr;
    }
    qbt::Log("virtual camera: registered");
    return S_OK;
}

void VirtualCamera::Stop() {
    if (!camera_) return;
    camera_->Shutdown(); // session lifetime: the device disappears with it
    camera_.Reset();
}
