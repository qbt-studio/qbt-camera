#include "VirtualCamera.h"

#include "../common/CameraIds.h"

namespace {

constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\QBT Camera Studio";
constexpr wchar_t kSystemCameraValue[] = L"SystemCamera";

HRESULT CreateCamera(MFVirtualCameraLifetime lifetime, MFVirtualCameraAccess access, IMFVirtualCamera** camera) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, lifetime, access, qbt::kVirtualCameraName,
                                 qbt::kVirtualCameraClsid, nullptr, 0, camera);
}

void SetSystemCameraFlag(bool installed) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (installed) {
        const DWORD one = 1;
        RegSetValueExW(key, kSystemCameraValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    } else {
        RegDeleteValueW(key, kSystemCameraValue);
    }
    RegCloseKey(key);
}

} // namespace

HRESULT VirtualCamera::Start() {
    if (camera_) return S_OK;
    if (IsSystemCameraInstalled()) {
        qbt::Log("virtual camera: system camera installed");
        return S_OK;
    }
    HRESULT hr = CreateCamera(MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser, &camera_);
    if (SUCCEEDED(hr)) hr = camera_->Start(nullptr);
    if (FAILED(hr)) {
        qbt::Log("virtual camera: start failed 0x%08lx", hr);
        camera_.Reset();
        return hr;
    }
    qbt::Log("virtual camera: session camera registered");
    return S_OK;
}

void VirtualCamera::Stop() {
    if (!camera_) return;
    camera_->Shutdown(); // session lifetime: the device disappears with it
    camera_.Reset();
}

HRESULT VirtualCamera::InstallSystemCamera() {
    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = CreateCamera(MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers, &camera);
    if (SUCCEEDED(hr)) hr = camera->Start(nullptr);
    if (camera) camera->Shutdown(); // releases our handle; a system camera stays registered
    if (SUCCEEDED(hr)) SetSystemCameraFlag(true);
    return hr;
}

HRESULT VirtualCamera::UninstallSystemCamera() {
    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = CreateCamera(MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers, &camera);
    if (SUCCEEDED(hr)) hr = camera->Remove();
    if (camera) camera->Shutdown();
    SetSystemCameraFlag(false);
    return hr;
}

bool VirtualCamera::IsSystemCameraInstalled() {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, kSystemCameraValue, RRF_RT_REG_DWORD, nullptr, &value, &bytes) ==
               ERROR_SUCCESS &&
           value == 1;
}
