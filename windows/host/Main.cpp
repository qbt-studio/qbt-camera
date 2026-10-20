// QBT Camera for Windows: receives the iPhone stream (USB or Wi-Fi), decodes it on the GPU,
// shows a preview and feeds the "QBT Camera" virtual webcam.
#include <winsock2.h>

#include <mfapi.h>
#include <shlobj.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "Decoder.h"
#include "PhoneLink.h"
#include "SharedFrameWriter.h"
#include "VideoPipeline.h"
#include "VirtualCamera.h"

namespace {

constexpr UINT kStatusMessage = WM_APP + 1;
constexpr wchar_t kWindowClass[] = L"QBTCameraHostWindow";

struct App {
    HWND window = nullptr;
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
    if (!g_app->virtualCameraReady) return L"виртуальная камера не установлена";
    return inUse ? L"камера используется приложением" : L"камера готова";
}

void StreamLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    App& app = *g_app;
    H264Decoder decoder;
    const HRESULT decoderStatus = decoder.Initialize(app.pipeline.Device());
    if (FAILED(decoderStatus)) {
        SetStatus(L"Ошибка: аппаратный декодер H.264 недоступен");
        CoUninitialize();
        return;
    }

    qbt::PacketHeader header{};
    std::vector<uint8_t> payload;
    while (!app.stop) {
        app.pipeline.Present(nullptr);
        SetStatus(L"Поиск iPhone (USB или Wi-Fi)…  ·  " + CameraState(false));
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
            if (header.type == qbt::kPacketTypeVideo) {
                DecodedFrame frame;
                const HRESULT hr = decoder.Decode(payload.data(), payload.size(),
                                                  static_cast<LONGLONG>(header.ptsMicros) * 10, frame);
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
            }
            bytes += sizeof(header) + payload.size();
            // Ack after the frame is on screen: if we fall behind, the phone slows down instead of queueing.
            if (!app.link.SendAck(++framesReceived)) break;

            const ULONGLONG now = GetTickCount64();
            if (now - windowStart >= 1000) {
                const double seconds = (now - windowStart) / 1000.0;
                wchar_t text[256];
                swprintf_s(text, L"%s  ·  %ldx%ld  ·  %.0f к/с  ·  %.1f Мбит/с  ·  %s", route.c_str(), width, height,
                           framesShown / seconds, bytes * 8 / seconds / 1e6,
                           CameraState(app.cameraOutput.CameraInUse()).c_str());
                SetStatus(text);
                framesShown = 0;
                bytes = 0;
                windowStart = now;
            }
        }
        app.link.Close();
        qbt::Log("stream ended");
    }
    CoUninitialize();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (g_app) g_app->pipeline.RequestResize();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case kStatusMessage: {
        std::wstring title;
        {
            std::lock_guard lock(g_app->statusMutex);
            title = L"QBT Camera  —  " + g_app->status;
        }
        SetWindowTextW(window, title.c_str());
        return 0;
    }
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

    const UINT dpi = GetDpiForSystem();
    RECT frame{0, 0, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96)};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    app.window = CreateWindowExW(0, kWindowClass, L"QBT Camera", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, nullptr);
    if (!app.window || FAILED(app.pipeline.Initialize(app.window))) {
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
