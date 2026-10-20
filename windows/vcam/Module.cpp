#include "Activator.h"

#include <string>

using Microsoft::WRL::InProc;
using Microsoft::WRL::Module;

CoCreatableClass(Activator);

namespace {

HMODULE g_module = nullptr;

std::wstring ClassKey() {
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + qbt::kVirtualCameraClsid;
}

HRESULT SetStringValue(HKEY key, const wchar_t* name, const std::wstring& value) {
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return HRESULT_FROM_WIN32(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes));
}

// Frame Server runs as LocalService, so it only sees machine-wide (HKLM) registrations.
HRESULT RegisterCameraClass(const std::wstring& dllPath) {
    HKEY classKey = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ClassKey().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &classKey, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    HRESULT hr = SetStringValue(classKey, nullptr, qbt::kVirtualCameraName);

    HKEY serverKey = nullptr;
    status = RegCreateKeyExW(classKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &serverKey, nullptr);
    if (status == ERROR_SUCCESS) {
        if (SUCCEEDED(hr)) hr = SetStringValue(serverKey, nullptr, dllPath);
        if (SUCCEEDED(hr)) hr = SetStringValue(serverKey, L"ThreadingModel", L"Both");
        RegCloseKey(serverKey);
    } else if (SUCCEEDED(hr)) {
        hr = HRESULT_FROM_WIN32(status);
    }
    RegCloseKey(classKey);
    return hr;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        // Frame Server runs as LocalService: its temp folder is
        // C:\Windows\ServiceProfiles\LocalService\AppData\Local\Temp.
        wchar_t temp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, temp)) qbt::SetLogFile(std::wstring(temp) + L"QBTVirtualCamera.log");
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    return Module<InProc>::GetModule().GetClassObject(clsid, riid, object);
}

STDAPI DllCanUnloadNow() {
    return Module<InProc>::GetModule().Terminate() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    return RegisterCameraClass(path);
}

STDAPI DllUnregisterServer() {
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClassKey().c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}
