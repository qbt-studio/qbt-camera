// QBT Camera for Windows: receives the iPhone stream (USB or Wi-Fi), decodes it on the GPU,
// shows a preview with camera controls and feeds the "QBT Camera" virtual webcam.
#include <winsock2.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "AudioOutput.h"
#include "Decoder.h"
#include "PhoneLink.h"
#include "SharedFrameWriter.h"
#include "Shell.h"
#include "StudioUi.h"
#include "Usbmux.h"
#include "VideoPipeline.h"
#include "VirtualCamera.h"

// Common Controls 6: modern tray menus and message boxes.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kPhoneMessage = WM_APP + 2;   // lParam: std::string* with JSON from the phone
constexpr UINT kDisconnectedMessage = WM_APP + 3;
constexpr UINT kTrayMessage = WM_APP + 4;
constexpr wchar_t kWindowClass[] = L"QBTCameraHostWindow";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\QBTCameraHostSingleInstance";
constexpr UINT_PTR kMeterTimer = 1;
constexpr UINT_PTR kDevicesTimer = 2;
constexpr UINT kMenuOpen = 1, kMenuAutostart = 2, kMenuExit = 3;
// After a failed USB attempt (e.g. this PC isn't trusted yet), stay on Wi-Fi this long.
constexpr ULONGLONG kUsbRetryDelayMs = 30'000;

struct App {
    HWND window = nullptr;
    HICON icon = nullptr;
    HICON smallIcon = nullptr;
    shell::TrayIcon tray;
    UINT taskbarCreatedMessage = 0;
    UINT showWindowMessage = 0; // sent by a second QBTCamera.exe instead of starting twice
    bool exiting = false;
    bool trayHintShown = false;
    std::atomic<bool> usbPresent{false};
    std::thread usbWatcher;
    StudioUi panel;
    VideoPipeline pipeline;
    PhoneLink link;
    AudioOutput audio;
    std::vector<AudioOutput::Device> audioDevices;
    std::wstring audioDevice;
    std::wstring settingsFolder;
    SharedFrameWriter cameraOutput;
    VirtualCamera virtualCamera;
    bool virtualCameraReady = false;
    std::atomic<bool> stop{false};
    std::thread worker;
    std::mutex statusMutex;
    HostStatus status;
    // Background effect as the phone reports it; composited here when the phone sends a matte stream.
    std::mutex backgroundMutex;
    BackgroundSettings background;
    bool backgroundOnPc = false;
};

App* g_app = nullptr;

void SetStatus(HostStatus status) {
    status.cameraInstalled = g_app->virtualCameraReady;
    {
        std::lock_guard lock(g_app->statusMutex);
        g_app->status = std::move(status);
    }
    PostMessageW(g_app->window, kStatusMessage, 0, 0);
}

HostStatus MakeStatus(HostStatus::Phase phase, const std::wstring& route = {}, bool usb = false) {
    HostStatus status;
    status.phase = phase;
    status.route = route;
    status.usb = usb;
    status.cameraInUse = g_app->cameraOutput.CameraInUse();
    return status;
}

void SleepUnlessStopped(DWORD milliseconds) {
    for (DWORD waited = 0; waited < milliseconds && !g_app->stop; waited += 50) Sleep(50);
}

void StreamLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    App& app = *g_app;
    H264Decoder decoder;
    H264Decoder auxDecoder; // the phone's matte stream for PC-side background effects
    if (FAILED(decoder.Initialize(app.pipeline.Device())) || FAILED(auxDecoder.Initialize(app.pipeline.Device()))) {
        HostStatus status;
        status.phase = HostStatus::Phase::Error;
        status.error = L"Аппаратный декодер H.264 недоступен";
        SetStatus(status);
        CoUninitialize();
        return;
    }

    qbt::PacketHeader header{};
    std::vector<uint8_t> payload;
    ULONGLONG usbRetryAfter = 0;
    while (!app.stop) {
        app.pipeline.SetIdleMessage(L"Ждём iPhone", L"Откройте QBT Camera на телефоне — подключение по USB или Wi-Fi");
        app.pipeline.Present(nullptr);
        SetStatus(MakeStatus(HostStatus::Phase::Searching));
        std::wstring route;
        bool usb = false;
        if (!app.link.Connect(app.stop, route, usb)) {
            SleepUnlessStopped(1000);
            continue;
        }
        // A cable is in but USB didn't work (PC not trusted yet, app just restarting): don't flap.
        if (!usb && app.usbPresent) usbRetryAfter = GetTickCount64() + kUsbRetryDelayMs;
        decoder.Flush();
        auxDecoder.Flush();
        DecodedFrame latestAux;
        uint64_t latestAuxPts = 0;
        ULONGLONG latestAuxTime = 0;
        SetStatus(MakeStatus(HostStatus::Phase::Connected, route, usb));

        uint32_t framesReceived = 0;
        bool phonePaused = false;
        ULONGLONG pausedKeepAlive = 0;
        int framesShown = 0;
        size_t bytes = 0;
        ULONGLONG windowStart = GetTickCount64();
        LONG width = 0, height = 0;
        while (!app.stop && app.link.ReadPacket(header, payload)) {
            if (!usb && app.usbPresent && GetTickCount64() >= usbRetryAfter) {
                qbt::Log("link: cable connected, switching to USB");
                break; // reconnect: Connect() tries USB first
            }
            bytes += sizeof(header) + payload.size();
            if (phonePaused && GetTickCount64() - pausedKeepAlive >= 250) {
                pausedKeepAlive = GetTickCount64();
                app.cameraOutput.KeepAlivePaused(); // audio keeps arriving: tell the camera why the picture stopped
            }
            if (header.type == qbt::kPacketTypeMessage) {
                const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
                if (text.find("\"evt\":\"state\"") != std::string_view::npos) {
                    const bool paused = text.find("\"paused\":true") != std::string_view::npos;
                    if (paused != phonePaused) {
                        phonePaused = paused;
                        qbt::Log("phone: picture %s", paused ? "paused (app in background)" : "resumed");
                        if (paused) {
                            app.pipeline.SetIdleMessage(L"Картинка на паузе",
                                                        L"Приложение на iPhone свёрнуто. Звук продолжает идти.");
                            app.pipeline.Present(nullptr);
                        }
                        SetStatus(MakeStatus(paused ? HostStatus::Phase::Paused : HostStatus::Phase::Connected, route, usb));
                    }
                }
                PostMessageW(app.window, kPhoneMessage, 0,
                             reinterpret_cast<LPARAM>(new std::string(payload.begin(), payload.end())));
                continue;
            }
            if (header.type == qbt::kPacketTypeAuxVideo) {
                DecodedFrame aux;
                if (auxDecoder.Decode(payload.data(), payload.size(), static_cast<LONGLONG>(header.ptsMicros) * 10, aux) == S_OK) {
                    aux.fullRange = true;
                    latestAux = std::move(aux);
                    latestAuxPts = header.ptsMicros;
                    latestAuxTime = GetTickCount64();
                }
                continue;
            }
            if (header.type == qbt::kPacketTypeAudio) {
                app.audio.Push(reinterpret_cast<const int16_t*>(payload.data()), payload.size() / sizeof(int16_t));
                continue;
            }
            if (header.type != qbt::kPacketTypeVideo) continue;

            DecodedFrame frame;
            const HRESULT hr = decoder.Decode(payload.data(), payload.size(), static_cast<LONGLONG>(header.ptsMicros) * 10, frame);
            if (hr == S_OK) {
                BackgroundSettings background;
                bool backgroundOnPc;
                {
                    std::lock_guard lock(app.backgroundMutex);
                    background = app.background;
                    backgroundOnPc = app.backgroundOnPc;
                }
                if (backgroundOnPc && background.mode != BackgroundSettings::Mode::Off) {
                    // The matte for this very frame arrives just before it (same timestamp).
                    const bool auxFresh = latestAux.texture && GetTickCount64() - latestAuxTime < 500;
                    if (auxFresh && latestAuxPts != header.ptsMicros) qbt::Log("background: matte out of step by %lld us",
                                                                                 static_cast<long long>(header.ptsMicros - latestAuxPts));
                    const DecodedFrame* aux = auxFresh ? &latestAux : nullptr;
                    if (aux || background.mode == BackgroundSettings::Mode::Chroma) {
                        DecodedFrame composited;
                        if (SUCCEEDED(app.pipeline.Composite(frame, aux, background, composited))) frame = std::move(composited);
                    }
                }
                app.pipeline.Present(&frame);
                if (app.cameraOutput.CameraInUse()) {
                    UINT cameraWidth = 0, cameraHeight = 0;
                    app.cameraOutput.RequestedSize(cameraWidth, cameraHeight);
                    app.pipeline.RenderNV12(frame, cameraWidth, cameraHeight, [&](const uint8_t* data, UINT pitch) {
                        app.cameraOutput.Write(data, pitch, cameraWidth, cameraHeight);
                    });
                }
                width = frame.visible.right - frame.visible.left;
                height = frame.visible.bottom - frame.visible.top;
                ++framesShown;
            } else if (FAILED(hr)) {
                qbt::Log("decode failed 0x%08lx", hr);
            }
            // Ack after the frame is on screen: if we fall behind, the phone slows down instead of queueing.
            if (!app.link.SendAck(++framesReceived)) break;

            const ULONGLONG now = GetTickCount64();
            if (now - windowStart >= 1000) {
                const double seconds = (now - windowStart) / 1000.0;
                HostStatus status = MakeStatus(HostStatus::Phase::Connected, route, usb);
                status.width = width;
                status.height = height;
                status.fps = framesShown / seconds;
                status.megabits = bytes * 8 / seconds / 1e6;
                SetStatus(status);
                framesShown = 0;
                bytes = 0;
                windowStart = now;
            }
        }
        app.link.Close();
        app.audio.Flush();
        PostMessageW(app.window, kDisconnectedMessage, 0, 0);
        qbt::Log("stream ended");
    }
    CoUninitialize();
}

void LayoutChildren(HWND window) {
    RECT client;
    GetClientRect(window, &client);
    MoveWindow(g_app->panel.Handle(), 0, 0, client.right, client.bottom, TRUE);
    // The preview card has 22 DIP corners; the video sits 1 DIP inside its rim.
    const float radius = 21.0f * static_cast<float>(GetDpiForWindow(window)) / 96.0f;
    g_app->pipeline.SetPreviewRect(g_app->panel.PreviewRect(), radius);
    g_app->pipeline.RefreshIdle();
}

void HandlePhoneMessage(const std::string& json) {
    JsonObject message;
    if (!JsonObject::Parse(json, message)) {
        qbt::Log("phone: unreadable message %.80s", json.c_str());
        return;
    }
    const std::string event = message.String("evt");
    if (event == "caps") {
        qbt::Log("phone: capabilities %s", json.c_str());
        if (const std::string wifi = message.String("wifi"); !wifi.empty()) g_app->link.RememberWifiAddress(wifi);
        g_app->panel.OnCapabilities(message);
    } else if (event == "state") {
        g_app->panel.OnState(message);
        // The phone is the source of truth for the background effect; the PC composites it when the
        // phone says so ("processing":"pc") and sends the matte stream.
        std::lock_guard lock(g_app->backgroundMutex);
        BackgroundSettings& background = g_app->background;
        const std::string mode = message.String("background");
        background.mode = mode == "portrait" ? BackgroundSettings::Mode::Portrait
                          : mode == "replace" ? BackgroundSettings::Mode::Replace
                          : mode == "chroma"  ? BackgroundSettings::Mode::Chroma
                                              : BackgroundSettings::Mode::Off;
        background.blur = static_cast<float>(message.Number("blur", 0.6));
        const std::string backdrop = message.String("backdrop");
        const char* const backdrops[] = {"studio", "glow", "warm", "white", "black", "green"};
        background.backdrop = 0;
        for (int i = 0; i < 6; ++i) {
            if (backdrop == backdrops[i]) background.backdrop = i;
        }
        background.keySensitivity = static_cast<float>(message.Number("keySensitivity", 0.5));
        background.keySoftness = static_cast<float>(message.Number("keySoftness", 0.4));
        background.keySpill = static_cast<float>(message.Number("keySpill", 0.7));
        g_app->backgroundOnPc = message.String("processing") == "pc";
    }
}

// The chosen audio device is remembered in %LOCALAPPDATA%\QBTCamera\audio-device.txt;
// "none" means the user chose not to play the sound anywhere.
std::wstring AudioSettingPath() {
    return g_app->settingsFolder + L"\\audio-device.txt";
}

bool LoadAudioSetting(std::wstring& deviceId) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, AudioSettingPath().c_str(), L"r, ccs=UTF-8") != 0 || !file) return false;
    wchar_t line[1024] = {};
    const bool read = fgetws(line, 1024, file) != nullptr;
    fclose(file);
    if (!read) return false;
    deviceId = line;
    while (!deviceId.empty() && (deviceId.back() == L'\n' || deviceId.back() == L'\r')) deviceId.pop_back();
    if (deviceId == L"none") deviceId.clear();
    return true;
}

void SaveAudioSetting(const std::wstring& deviceId) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, AudioSettingPath().c_str(), L"w, ccs=UTF-8") != 0 || !file) return;
    fputws(deviceId.empty() ? L"none" : deviceId.c_str(), file);
    fclose(file);
}

void SelectAudioDevice(const std::wstring& deviceId) {
    g_app->audioDevice = deviceId;
    g_app->audio.Start(deviceId);
}

/// Re-reads playback devices (VB-Cable may get installed while we run) and keeps the choice valid.
void RefreshAudioDevices(bool firstTime) {
    App& app = *g_app;
    auto devices = AudioOutput::ListDevices();
    if (!firstTime && devices == app.audioDevices) return;
    app.audioDevices = std::move(devices);

    std::wstring wanted;
    const bool saved = LoadAudioSetting(wanted);
    if (!saved || (!wanted.empty() && std::none_of(app.audioDevices.begin(), app.audioDevices.end(),
                                                   [&](const auto& d) { return d.first == wanted; }))) {
        // Default: VB-Audio Virtual Cable if present — it turns our sound into a microphone for calls.
        // Never default to speakers: the call would hear itself.
        wanted.clear();
        for (const auto& [id, name] : app.audioDevices) {
            if (name.find(L"CABLE Input") != std::wstring::npos) wanted = id;
        }
    }
    if (firstTime || wanted != app.audioDevice) SelectAudioDevice(wanted);
    app.panel.SetAudioDevices(app.audioDevices, app.audioDevice);
}

void ShowMainWindow() {
    ShowWindow(g_app->window, IsIconic(g_app->window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_app->window);
}

void ShowTrayMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kMenuOpen, L"Открыть QBT Camera");
    AppendMenuW(menu, MF_STRING | (shell::IsAutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart,
                L"Запускать вместе с Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Выход");
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(window); // so the menu closes when clicking elsewhere
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);
    switch (choice) {
    case kMenuOpen:
        ShowMainWindow();
        break;
    case kMenuAutostart:
        shell::SetAutostart(!shell::IsAutostartEnabled());
        break;
    case kMenuExit:
        g_app->exiting = true;
        PostMessageW(window, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_app && message == g_app->showWindowMessage && message != 0) {
        ShowMainWindow();
        return 0;
    }
    if (g_app && message == g_app->taskbarCreatedMessage && message != 0) {
        g_app->tray.Restore();
        return 0;
    }
    // The system minimize/maximize/close buttons live inside our client area (custom frame).
    LRESULT dwmResult = 0;
    if (DwmDefWindowProc(window, message, wParam, lParam, &dwmResult)) return dwmResult;
    switch (message) {
    case WM_NCCALCSIZE:
        if (wParam) {
            // Keep the side and bottom resize borders, drop the title bar: the header takes its place.
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const LONG top = params->rgrc[0].top;
            DefWindowProcW(window, message, wParam, lParam);
            params->rgrc[0].top = top;
            if (IsZoomed(window)) {
                const UINT dpi = GetDpiForWindow(window);
                params->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
        if (hit != HTCLIENT || !g_app) return hit;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        const UINT dpi = GetDpiForWindow(window);
        const int border = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        if (!IsZoomed(window) && point.y < border) return HTTOP;
        if (point.y < g_app->panel.HeaderHeight()) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
            g_app->panel.OnSystemThemeChanged();
        break;
    case kTrayMessage:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) ShowMainWindow();
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu(window);
        return 0;
    case WM_SIZE:
        if (g_app && g_app->panel.Handle()) LayoutChildren(window);
        return 0;
    case WM_SETFOCUS:
        if (g_app && g_app->panel.Handle()) SetFocus(g_app->panel.Handle());
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const int dpi = static_cast<int>(GetDpiForWindow(window));
        limits->ptMinTrackSize = {MulDiv(1060, dpi, 96), MulDiv(700, dpi, 96)};
        return 0;
    }
    case kStatusMessage: {
        HostStatus status;
        {
            std::lock_guard lock(g_app->statusMutex);
            status = g_app->status;
        }
        g_app->panel.SetStatus(status);
        std::wstring tip = L"QBT Camera — ";
        switch (status.phase) {
        case HostStatus::Phase::Connected: tip += L"в эфире: " + status.route; break;
        case HostStatus::Phase::Paused: tip += L"картинка на паузе"; break;
        case HostStatus::Phase::Error: tip += status.error; break;
        default: tip += L"ждём iPhone"; break;
        }
        g_app->tray.SetTip(tip.c_str());
        return 0;
    }
    case kPhoneMessage: {
        std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lParam));
        HandlePhoneMessage(*json);
        return 0;
    }
    case kDisconnectedMessage:
        g_app->panel.OnDisconnected();
        return 0;
    case WM_TIMER:
        if (wParam == kMeterTimer) g_app->panel.SetAudioLevel(g_app->audio.TakePeak());
        if (wParam == kDevicesTimer) RefreshAudioDevices(false);
        return 0;
    case WM_CLOSE:
        if (!g_app->exiting) {
            // Like Camo: closing the window keeps the camera working from the tray.
            ShowWindow(window, SW_HIDE);
            if (!g_app->trayHintShown) {
                g_app->trayHintShown = true;
                g_app->tray.ShowBalloon(L"QBT Camera работает в фоне",
                                        L"Камера доступна в приложениях. Откройте окно или выйдите через значок в трее.");
            }
            return 0;
        }
        g_app->stop = true;
        g_app->link.Close();
        if (g_app->worker.joinable()) g_app->worker.join();
        if (g_app->usbWatcher.joinable()) g_app->usbWatcher.join();
        g_app->audio.Stop();
        g_app->tray.Remove();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/// %LOCALAPPDATA%\QBTCamera: log and settings.
std::wstring SettingsFolder() {
    PWSTR localAppData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        path = std::wstring(localAppData) + L"\\QBTCamera";
        CreateDirectoryW(path.c_str(), nullptr);
    }
    CoTaskMemFree(localAppData);
    return path;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    // Installer modes (run elevated by windows/install_camera.ps1).
    const std::wstring arguments = commandLine ? commandLine : L"";
    if (arguments.find(L"--install-camera") != std::wstring::npos ||
        arguments.find(L"--uninstall-camera") != std::wstring::npos) {
        const bool install = arguments.find(L"--install-camera") != std::wstring::npos;
        const HRESULT hr = install ? VirtualCamera::InstallSystemCamera() : VirtualCamera::UninstallSystemCamera();
        MFShutdown();
        CoUninitialize();
        return SUCCEEDED(hr) ? 0 : static_cast<int>(hr);
    }

    // One QBT Camera at a time: a second launch just brings the running one to the front.
    const UINT showWindowMessage = RegisterWindowMessageW(L"QBTCameraShowWindow");
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND running = FindWindowW(kWindowClass, nullptr)) PostMessageW(running, showWindowMessage, 0, 0);
        return 0;
    }
    const bool startMinimized = arguments.find(L"--minimized") != std::wstring::npos;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    App app;
    g_app = &app;
    app.showWindowMessage = showWindowMessage;
    app.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    app.settingsFolder = SettingsFolder();
    qbt::SetLogFile(app.settingsFolder + L"\\host.log");
    qbt::Log("QBT Camera started");
    app.icon = shell::MakeLogoIcon(GetSystemMetrics(SM_CXICON) * 2);
    app.smallIcon = shell::MakeLogoIcon(GetSystemMetrics(SM_CXSMICON) * 2);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = app.icon;
    windowClass.hIconSm = app.smallIcon;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    const UINT dpi = GetDpiForSystem();
    RECT frame{0, 0, MulDiv(1360, dpi, 96), MulDiv(820, dpi, 96)};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    // No redirection bitmap: everything inside is DirectComposition, so the Mica material shows through.
    app.window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kWindowClass, L"QBT Camera", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr,
                                 nullptr, instance, nullptr);
    // Windows 11 material behind the whole window, the header drawn where the title bar was.
    const MARGINS glass{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(app.window, &glass);
    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    const bool mica = SUCCEEDED(DwmSetWindowAttribute(app.window, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(app.window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    SetWindowPos(app.window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    if (FAILED(app.pipeline.Initialize(app.window))) {
        MessageBoxW(nullptr, L"Не удалось инициализировать видеокарту (Direct3D 11).", L"QBT Camera", MB_ICONERROR);
        return 1;
    }
    app.panel.Create(
        app.window, instance, app.settingsFolder,
        [](const std::string& json) {
            qbt::Log("command %s", json.c_str());
            if (json.find("keyResample") != std::string::npos) { // chroma key composited here: sample again
                std::lock_guard lock(g_app->backgroundMutex);
                ++g_app->background.keyGeneration;
            }
            g_app->link.SendCommand(json);
        },
        [](const std::wstring& deviceId) {
            SaveAudioSetting(deviceId);
            SelectAudioDevice(deviceId);
        },
        [](bool dark) {
            // Caption buttons and the Mica tint follow the app's theme.
            const BOOL darkMode = dark ? TRUE : FALSE;
            DwmSetWindowAttribute(g_app->window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
            g_app->pipeline.SetIdleTheme(dark);
        },
        [](IUnknown* swapChain) {
            if (FAILED(g_app->pipeline.AddUnderlay(swapChain))) qbt::Log("ui: could not add the interface layer");
        });
    app.panel.SetBackdropAvailable(mica);
    LayoutChildren(app.window);
    RefreshAudioDevices(true);
    SetTimer(app.window, kMeterTimer, 50, nullptr);
    SetTimer(app.window, kDevicesTimer, 3000, nullptr);

    app.virtualCameraReady = SUCCEEDED(app.virtualCamera.Start());
    SetStatus(MakeStatus(HostStatus::Phase::Starting));
    app.tray.Add(app.window, kTrayMessage, app.smallIcon, L"QBT Camera");
    // Cheap localhost query: notices a plugged-in cable within ~1.5 s.
    app.usbWatcher = std::thread([] {
        while (!g_app->stop) {
            g_app->usbPresent = !UsbmuxListDevices().empty();
            SleepUnlessStopped(1500);
        }
    });
    app.worker = std::thread(StreamLoop);
    if (!startMinimized) ShowWindow(app.window, showCommand);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    app.virtualCamera.Stop();
    g_app = nullptr;
    WSACleanup();
    MFShutdown();
    CoUninitialize();
    if (singleInstance) CloseHandle(singleInstance);
    return 0;
}
