// QBT Camera for Windows: receives the iPhone stream (USB or Wi-Fi), decodes it on the GPU,
// shows a preview with camera controls and feeds the "QBT Camera" virtual webcam.
#include <winsock2.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <shlobj.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ControlPanel.h"
#include "Decoder.h"
#include "PhoneLink.h"
#include "SharedFrameWriter.h"
#include "VideoPipeline.h"
#include "VirtualCamera.h"

// Common Controls 6: themed trackbars and buttons.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kPhoneMessage = WM_APP + 2;   // lParam: std::string* with JSON from the phone
constexpr UINT kDisconnectedMessage = WM_APP + 3;
constexpr wchar_t kWindowClass[] = L"QBTCameraHostWindow";
constexpr wchar_t kPreviewClass[] = L"QBTCameraPreview";

struct App {
    HWND window = nullptr;
    HWND preview = nullptr;
    ControlPanel panel;
    VideoPipeline pipeline;
    PhoneLink link;
    SharedFrameWriter cameraOutput;
    VirtualCamera virtualCamera;
    bool virtualCameraReady = false;
    std::atomic<bool> stop{false};
    std::thread worker;
    std::mutex statusMutex;
    std::wstring status;
};

App* g_app = nullptr;

void SetStatus(std::wstring text) {
    {
        std::lock_guard lock(g_app->statusMutex);
        g_app->status = std::move(text);
    }
    PostMessageW(g_app->window, kStatusMessage, 0, 0);
}

void SleepUnlessStopped(DWORD milliseconds) {
    for (DWORD waited = 0; waited < milliseconds && !g_app->stop; waited += 50) Sleep(50);
}

std::wstring CameraState(bool inUse) {
    if (!g_app->virtualCameraReady) return L"Виртуальная камера не установлена";
    return inUse ? L"Камера «QBT Camera» используется" : L"Камера «QBT Camera» готова";
}

void StreamLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    App& app = *g_app;
    H264Decoder decoder;
    if (FAILED(decoder.Initialize(app.pipeline.Device()))) {
        SetStatus(L"Ошибка: аппаратный декодер H.264 недоступен");
        CoUninitialize();
        return;
    }

    qbt::PacketHeader header{};
    std::vector<uint8_t> payload;
    while (!app.stop) {
        app.pipeline.Present(nullptr);
        SetStatus(L"Поиск iPhone по USB и Wi-Fi…\nОткройте QBT Camera на телефоне.\n" + CameraState(false));
        std::wstring route;
        if (!app.link.Connect(app.stop, route)) {
            SleepUnlessStopped(1000);
            continue;
        }
        decoder.Flush();
        SetStatus(L"Подключено: " + route);

        uint32_t framesReceived = 0;
        int framesShown = 0;
        size_t bytes = 0;
        ULONGLONG windowStart = GetTickCount64();
        LONG width = 0, height = 0;
        while (!app.stop && app.link.ReadPacket(header, payload)) {
            bytes += sizeof(header) + payload.size();
            if (header.type == qbt::kPacketTypeMessage) {
                PostMessageW(app.window, kPhoneMessage, 0,
                             reinterpret_cast<LPARAM>(new std::string(payload.begin(), payload.end())));
                continue;
            }
            if (header.type != qbt::kPacketTypeVideo) continue;

            DecodedFrame frame;
            const HRESULT hr = decoder.Decode(payload.data(), payload.size(), static_cast<LONGLONG>(header.ptsMicros) * 10, frame);
            if (hr == S_OK) {
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
                wchar_t text[320];
                swprintf_s(text, L"Подключено: %s\n%ld×%ld  ·  %.0f к/с  ·  %.1f Мбит/с\n%s", route.c_str(), width, height,
                           framesShown / seconds, bytes * 8 / seconds / 1e6,
                           CameraState(app.cameraOutput.CameraInUse()).c_str());
                SetStatus(text);
                framesShown = 0;
                bytes = 0;
                windowStart = now;
            }
        }
        app.link.Close();
        PostMessageW(app.window, kDisconnectedMessage, 0, 0);
        qbt::Log("stream ended");
    }
    CoUninitialize();
}

void LayoutChildren(HWND window) {
    RECT client;
    GetClientRect(window, &client);
    const int panelWidth = MulDiv(ControlPanel::kWidth, static_cast<int>(GetDpiForWindow(window)), 96);
    const int previewWidth = std::max<int>(client.right - panelWidth, 0);
    MoveWindow(g_app->preview, 0, 0, previewWidth, client.bottom, TRUE);
    MoveWindow(g_app->panel.Handle(), previewWidth, 0, client.right - previewWidth, client.bottom, TRUE);
    g_app->pipeline.RequestResize();
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
        g_app->panel.OnCapabilities(message);
    } else if (event == "state") {
        g_app->panel.OnState(message);
    }
}

LRESULT CALLBACK PreviewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) return 1; // the swap chain paints everything
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (g_app && g_app->preview) LayoutChildren(window);
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const int dpi = static_cast<int>(GetDpiForWindow(window));
        limits->ptMinTrackSize = {MulDiv(860, dpi, 96), MulDiv(640, dpi, 96)};
        return 0;
    }
    case kStatusMessage: {
        std::wstring status;
        {
            std::lock_guard lock(g_app->statusMutex);
            status = g_app->status;
        }
        g_app->panel.SetStatus(status);
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
    case WM_CLOSE:
        g_app->stop = true;
        g_app->link.Close();
        if (g_app->worker.joinable()) g_app->worker.join();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

std::wstring LogFilePath() {
    PWSTR localAppData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        path = std::wstring(localAppData) + L"\\QBTCamera";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\host.log";
    }
    CoTaskMemFree(localAppData);
    return path;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    qbt::SetLogFile(LogFilePath());
    qbt::Log("QBT Camera started");

    App app;
    g_app = &app;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    WNDCLASSEXW previewClass{sizeof(previewClass)};
    previewClass.lpfnWndProc = PreviewProc;
    previewClass.hInstance = instance;
    previewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    previewClass.lpszClassName = kPreviewClass;
    RegisterClassExW(&previewClass);

    const UINT dpi = GetDpiForSystem();
    RECT frame{0, 0, MulDiv(1280 + ControlPanel::kWidth, dpi, 96), MulDiv(760, dpi, 96)};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    app.window = CreateWindowExW(0, kWindowClass, L"QBT Camera", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
                                 instance, nullptr);
    const BOOL darkTitleBar = TRUE;
    DwmSetWindowAttribute(app.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitleBar, sizeof(darkTitleBar));
    app.preview = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, app.window, nullptr,
                                  instance, nullptr);
    app.panel.Create(app.window, instance, [](const std::string& json) {
        qbt::Log("command %s", json.c_str());
        g_app->link.SendCommand(json);
    });
    LayoutChildren(app.window);
    if (!app.preview || FAILED(app.pipeline.Initialize(app.preview))) {
        MessageBoxW(nullptr, L"Не удалось инициализировать видеокарту (Direct3D 11).", L"QBT Camera", MB_ICONERROR);
        return 1;
    }

    app.virtualCameraReady = SUCCEEDED(app.virtualCamera.Start());
    app.worker = std::thread(StreamLoop);
    ShowWindow(app.window, showCommand);

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
    return 0;
}
