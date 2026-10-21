#include "StudioUi.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "../common/Log.h"
#include "Shell.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"QBTStudioUi";
constexpr UINT_PTR kFlushTimer = 1;
constexpr UINT_PTR kAnimationTimer = 2;
constexpr UINT_PTR kPulseTimer = 3;
constexpr ULONGLONG kSendIntervalMs = 33;
// After the user moves a control, ignore the phone's live value for it for a moment,
// otherwise the slider would jump back under the mouse.
constexpr ULONGLONG kTouchHoldMs = 1500;

// Layout, in DIPs.
constexpr float kPad = 20, kHeader = 68, kStrip = 110, kCardRadius = 22;
// Room for the system minimize/maximize/close buttons in the top right corner.
constexpr float kCaptionButtons = 150;

constexpr D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.0f) {
    return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
}
constexpr D2D1_COLOR_F White(float alpha) { return {1, 1, 1, alpha}; }
constexpr D2D1_COLOR_F Black(float alpha) { return {0, 0, 0, alpha}; }
constexpr D2D1_COLOR_F Ink(float alpha) { return Rgb(0x0E1324, alpha); }

// Status colors (same in both themes).
constexpr D2D1_COLOR_F kGreen = Rgb(0x22C55E);
constexpr D2D1_COLOR_F kAmber = Rgb(0xF59E0B);
constexpr D2D1_COLOR_F kRed = Rgb(0xEF4444);
constexpr D2D1_COLOR_F kCyan = Rgb(0x06B6D4);
constexpr D2D1_COLOR_F kBlue = Rgb(0x2B7FFF);

// Segoe Fluent Icons (built into Windows 11).
constexpr wchar_t kIconCamera = 0xE722, kIconVideo = 0xE714, kIconMicrophone = 0xE720, kIconWifi = 0xE701,
                  kIconUsb = 0xE88E, kIconTorch = 0xE754, kIconMirror = 0xE8AB, kIconSparkle = 0xF4A5, kIconBolt = 0xE945,
                  kIconThermometer = 0xE9CA, kIconMonitor = 0xE7F4, kIconTimer = 0xE916, kIconPulse = 0xE9D9,
                  kIconSun = 0xE706, kIconMoon = 0xE708, kIconAutoTheme = 0xE793, kIconWarning = 0xE7BA;

// Widget ids.
enum : int {
    kTabBase = 10,
    kLensBase = 20,
    kMirrorButton = 30, kTorchButton, kThemeButton,
    kExposureAuto = 40, kIso, kShutter, kBias, kWhiteBalanceAuto, kKelvin, kTint, kFocusAuto, kFocus, kZoom, kTorch,
    kBlur = 60, kRetouch, kFraming,
    kFilterBase = 70,
    kFilterAmount = 80, kMirrorSwitch,
    kMicrophone = 90,
    kDeviceBase = 100,
    kResolutionBase = 120,
    kFpsBase = 130,
    kOrientationBase = 140,
    kBlackout = 150, kPrompterVisible, kPrompterPlaying, kPrompterSpeed, kAutostart,
    kBackgroundBase = 160,
    kBackdropBase = 170,
    kKeySensitivity = 180, kKeySoftness, kKeySpill, kKeyResample,
    kProcessingBase = 190,
    // Animation slots that aren't widgets.
    kTabIndicatorX = 5000, kLensIndicatorX, kLensIndicatorWidth, kScroll, kThemeBlend,
};
constexpr int kHoverOffset = 1000, kSwitchOffset = 2000, kSelectOffset = 3000;

const char* const kLensNames[4] = {"ultrawide", "wide", "tele", "front"};
const char* const kFilterNames[8] = {"none", "warm", "cool", "cinema", "vivid", "soft", "mono", "noir"};
const wchar_t* const kFilterTitles[8] = {L"Нет", L"Тёплый", L"Холодный", L"Кино", L"Яркий", L"Мягкий", L"Ч/Б", L"Нуар"};
const char* const kResolutionNames[3] = {"hd720", "fullHD", "uhd4K"};
const wchar_t* const kResolutionTitles[3] = {L"720p", L"1080p", L"4K"};
constexpr int kFrameRates[2] = {30, 60};
const char* const kOrientationNames[4] = {"auto", "landscape", "landscapeFlipped", "portrait"};
const wchar_t* const kOrientationTitles[4] = {L"Авто", L"Горизонтально", L"Перевёрнуто", L"Вертикально"};
// Shutter speeds in microseconds: 1/8000 … 1/15 (1/50 and 1/100 avoid flicker under 50 Hz lights).
constexpr double kShutterStops[] = {125, 250, 500, 1000, 2000, 4000, 5000, 8000, 10000, 16667, 20000, 33333, 40000, 66667};
constexpr double kMinKelvin = 2500, kMaxKelvin = 8000;
const char* const kBackgroundNames[4] = {"off", "portrait", "replace", "chroma"};
const wchar_t* const kBackgroundTitles[4] = {L"Выкл", L"Портрет", L"Замена", L"Хромакей"};
const char* const kBackdropNames[6] = {"studio", "glow", "warm", "white", "black", "green"};
const wchar_t* const kBackdropTitles[6] = {L"Студия", L"QBT", L"Тёплый", L"Белый", L"Чёрный", L"Зелёный"};

// Rows inside the grouped settings, in DIPs.
constexpr float kRowPad = 14, kSliderRow = 58, kSwitchRow = 46, kHintRow = 58, kChipHeight = 34, kGroupTitle = 26,
                kGroupGap = 18;

std::string Format(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

std::wstring WideFormat(const wchar_t* format, ...) {
    wchar_t buffer[256];
    va_list args;
    va_start(args, format);
    vswprintf_s(buffer, format, args);
    va_end(args);
    return buffer;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

D2D1_RECT_F Inflate(const D2D1_RECT_F& rect, float amount) {
    return {rect.left - amount, rect.top - amount, rect.right + amount, rect.bottom + amount};
}

D2D1_COLOR_F Mix(D2D1_COLOR_F from, D2D1_COLOR_F to, float t) {
    return {from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t,
            from.a + (to.a - from.a) * t};
}

D2D1_COLOR_F Faded(D2D1_COLOR_F color, float alpha) {
    color.a *= alpha;
    return color;
}

bool SystemUsesLightTheme() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value != 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Themes

StudioUi::Palette StudioUi::DarkPalette() {
    Palette p;
    p.base = Rgb(0x151A28);
    p.wash = Rgb(0x0F1424, 0.50f); // tints Mica towards deep navy
    p.blobA = Rgb(0x3A86FF, 0.32f);
    p.blobB = Rgb(0x8B5CF6, 0.24f);
    p.blobC = Rgb(0x22D3EE, 0.10f);
    p.glassFill = White(0.07f);
    p.glassSheen = White(0.07f);
    p.glassBorder = White(0.10f);
    p.glassRim = White(0.30f);
    p.shadow = Black(0.45f);
    p.platter = White(0.055f);
    p.platterBorder = White(0.06f);
    p.separator = White(0.07f);
    p.text = White(0.96f);
    p.secondary = White(0.64f);
    p.tertiary = White(0.40f);
    p.selectedFill = White(0.20f);
    p.selectedText = White(1.0f);
    p.control = White(0.08f);
    p.hover = White(0.06f);
    p.track = White(0.16f);
    p.accent = Rgb(0x3A86FF);
    p.accentSoft = Rgb(0x3A86FF, 0.22f);
    return p;
}

StudioUi::Palette StudioUi::LightPalette() {
    Palette p;
    p.base = Rgb(0xEEF1F8);
    p.wash = Rgb(0xF5F7FC, 0.42f);
    p.blobA = Rgb(0x7DB4FF, 0.46f);
    p.blobB = Rgb(0xC4B5FD, 0.42f);
    p.blobC = Rgb(0xFDBA8C, 0.26f);
    p.glassFill = White(0.58f);
    p.glassSheen = White(0.55f);
    p.glassBorder = White(0.75f);
    p.glassRim = White(1.0f);
    p.shadow = Rgb(0x23304F, 0.16f);
    p.platter = White(0.62f);
    p.platterBorder = Ink(0.05f);
    p.separator = Ink(0.07f);
    p.text = Ink(0.92f);
    p.secondary = Ink(0.58f);
    p.tertiary = Ink(0.38f);
    p.selectedFill = White(1.0f);
    p.selectedText = Ink(0.95f);
    p.control = Ink(0.045f);
    p.hover = Ink(0.04f);
    p.track = Ink(0.10f);
    p.accent = Rgb(0x2B7FFF);
    p.accentSoft = Rgb(0x2B7FFF, 0.14f);
    return p;
}

StudioUi::Palette StudioUi::Blend(const Palette& from, const Palette& to, float t) {
    static_assert(sizeof(Palette) % sizeof(Color) == 0, "Palette must be colors only");
    Palette out;
    const auto* a = reinterpret_cast<const Color*>(&from);
    const auto* b = reinterpret_cast<const Color*>(&to);
    auto* result = reinterpret_cast<Color*>(&out);
    for (size_t i = 0; i < sizeof(Palette) / sizeof(Color); ++i) result[i] = Mix(a[i], b[i], t);
    return out;
}

void StudioUi::ApplyTheme(bool animate) {
    dark_ = themeMode_ == ThemeMode::Dark || (themeMode_ == ThemeMode::Auto && !SystemUsesLightTheme());
    if (!animate) animations_[kThemeBlend] = dark_ ? 0.0f : 1.0f;
    if (themeSink_) themeSink_(dark_);
    Invalidate();
}

void StudioUi::SaveThemeMode() {
    FILE* file = nullptr;
    if (_wfopen_s(&file, (settingsFolder_ + L"\\theme.txt").c_str(), L"w") != 0 || !file) return;
    fputs(themeMode_ == ThemeMode::Light ? "light" : themeMode_ == ThemeMode::Dark ? "dark" : "auto", file);
    fclose(file);
}

void StudioUi::OnSystemThemeChanged() {
    if (themeMode_ == ThemeMode::Auto) ApplyTheme(true);
}

void StudioUi::SetBackdropAvailable(bool available) {
    backdrop_ = available;
    Invalidate();
}

// ---------------------------------------------------------------------------------------------
// Window, device and resources

bool StudioUi::Create(HWND parent, HINSTANCE instance, const std::wstring& settingsFolder, CommandSink commands,
                      DeviceSink audioDevice, ThemeSink theme, SurfaceSink surface) {
    parent_ = parent;
    settingsFolder_ = settingsFolder;
    sink_ = std::move(commands);
    deviceSink_ = std::move(audioDevice);
    themeSink_ = std::move(theme);
    surfaceSink_ = std::move(surface);

    FILE* file = nullptr;
    if (_wfopen_s(&file, (settingsFolder_ + L"\\theme.txt").c_str(), L"r") == 0 && file) {
        char mode[16] = {};
        fgets(mode, sizeof(mode), file);
        fclose(file);
        themeMode_ = strncmp(mode, "light", 5) == 0 ? ThemeMode::Light : strncmp(mode, "dark", 4) == 0 ? ThemeMode::Dark
                                                                                                      : ThemeMode::Auto;
    }

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    // Input only: everything is drawn into the composition swap chain.
    window_ = CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, parent, nullptr, instance, this);
    if (!window_) return false;
    dpi_ = GetDpiForWindow(window_);

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    const auto format = [this](ComPtr<IDWriteTextFormat>& out, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                               bool wrap = false) {
        dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                  L"ru-ru", &out);
        if (out) out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    format(titleFormat_, L"Segoe UI Variable Display", 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format(bodyFormat_, L"Segoe UI Variable Text", 13.5f, DWRITE_FONT_WEIGHT_NORMAL);
    format(strongFormat_, L"Segoe UI Variable Text", 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format(smallFormat_, L"Segoe UI Variable Text", 12, DWRITE_FONT_WEIGHT_NORMAL);
    format(captionFormat_, L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format(bigFormat_, L"Segoe UI Variable Display", 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format(groupFormat_, L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format(wrapFormat_, L"Segoe UI Variable Text", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);

    ApplyTheme(false);
    return CreateDevice();
}

bool StudioUi::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, nullptr)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, nullptr)))
        return false;
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) return false;

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))) ||
        FAILED(factory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &target_)))
        return false;
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->CreateSolidColorBrush(White(1), &brush_);

    // White fading to transparent: tinted per use through the brush opacity.
    const D2D1_GRADIENT_STOP fade[] = {{0.0f, White(1)}, {1.0f, White(0)}};
    ComPtr<ID2D1GradientStopCollection> stops;
    target_->CreateGradientStopCollection(fade, 2, &stops);
    if (stops) {
        target_->CreateLinearGradientBrush({}, stops.Get(), &sheenBrush_);
        target_->CreateLinearGradientBrush({}, stops.Get(), &rimBrush_);
    }
    return brush_ && sheenBrush_ && rimBrush_;
}

bool StudioUi::ResizeSwapChain(UINT width, UINT height) {
    width = std::max(width, 8u);
    height = std::max(height, 8u);
    if (swapChain_ && width == bufferWidth_ && height == bufferHeight_) return true;
    target_->SetTarget(nullptr);
    if (!swapChain_) {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
            return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // transparent where Mica should show
        if (const HRESULT hr = factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapChain_); FAILED(hr)) {
            qbt::Log("ui: swap chain failed 0x%08lx", hr);
            return false;
        }
        if (surfaceSink_) surfaceSink_(swapChain_.Get());
    } else if (FAILED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
        return false;
    }
    bufferWidth_ = width;
    bufferHeight_ = height;
    return true;
}

LRESULT CALLBACK StudioUi::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        reinterpret_cast<StudioUi*>(create->lpCreateParams)->window_ = window;
    }
    auto* self = reinterpret_cast<StudioUi*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT StudioUi::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    const auto toDips = [this](LPARAM position) {
        return D2D1::Point2F(GET_X_LPARAM(position) * 96.0f / Dpi(), GET_Y_LPARAM(position) * 96.0f / Dpi());
    };
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(window_, &paint);
        Frame();
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        // The header is the title bar: let the parent handle dragging and the caption buttons,
        // except over our own header controls.
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window_, &point);
        const float x = point.x * 96.0f / Dpi(), y = point.y * 96.0f / Dpi();
        if (y < kHeader) {
            const bool control = std::any_of(headerControls_.begin(), headerControls_.end(), [&](const Rect& r) {
                return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
            });
            if (!control) return HTTRANSPARENT;
        }
        return HTCLIENT;
    }
    case WM_SIZE:
        Invalidate();
        return 0;
    case WM_MOUSEMOVE: {
        mouse_ = toDips(lParam);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
        Invalidate();
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!mouseDown_) mouse_ = {-1000, -1000};
        Invalidate();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(window_);
        SetCapture(window_);
        mouse_ = toDips(lParam);
        mouseDown_ = true;
        pressed_ = true;
        Invalidate();
        UpdateWindow(window_);
        return 0;
    case WM_LBUTTONUP:
        mouse_ = toDips(lParam);
        mouseDown_ = false;
        released_ = true;
        ReleaseCapture();
        Invalidate();
        UpdateWindow(window_);
        return 0;
    case WM_MOUSEWHEEL:
        wheel_ += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam));
        Invalidate();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, hot_ || active_ ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wParam == kFlushTimer) Flush();
        if (wParam == kAnimationTimer || wParam == kPulseTimer) Invalidate();
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        dpi_ = GetDpiForWindow(window_);
        shadowShapes_.clear();
        Invalidate();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

StudioUi::Layout StudioUi::ComputeLayout(float width, float height) const {
    const float sidebarWidth = std::clamp(width * 0.3f, 350.0f, 410.0f);
    Layout layout;
    layout.header = {kPad, 0, width - kPad, kHeader};
    layout.sidebar = {width - kPad - sidebarWidth, kHeader, width - kPad, height - kPad};
    const float mainRight = layout.sidebar.left - 18;
    layout.strip = {kPad, height - kPad - kStrip, mainRight, height - kPad};
    layout.card = {kPad, kHeader, mainRight, layout.strip.top - 16};
    return layout;
}

RECT StudioUi::PreviewRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    const float scale = Dpi() / 96.0f;
    const Layout layout = ComputeLayout(client.right / scale, client.bottom / scale);
    const Rect card = Inflate(layout.card, -1);
    return {static_cast<LONG>(std::lround(card.left * scale)), static_cast<LONG>(std::lround(card.top * scale)),
            static_cast<LONG>(std::lround(card.right * scale)), static_cast<LONG>(std::lround(card.bottom * scale))};
}

int StudioUi::HeaderHeight() const {
    return static_cast<int>(std::lround(kHeader * Dpi() / 96.0f));
}

// ---------------------------------------------------------------------------------------------
// Frame

void StudioUi::Frame() {
    if (!target_) return;
    RECT client{};
    GetClientRect(window_, &client);
    if (!ResizeSwapChain(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom))) return;

    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    deltaSeconds_ = lastFrame_.QuadPart
                        ? std::clamp(static_cast<float>(now.QuadPart - lastFrame_.QuadPart) / frequency.QuadPart, 0.0f, 0.1f)
                        : 0.016f;
    lastFrame_ = now;
    time_ += deltaSeconds_;
    animating_ = false;
    wantsPulse_ = false;
    nextHot_ = 0;
    headerControls_.clear();
    palette_ = Blend(DarkPalette(), LightPalette(), Animate(kThemeBlend, dark_ ? 0.0f : 1.0f, 7));

    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), Dpi(), Dpi());
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(target_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap))) return;
    target_->SetTarget(bitmap.Get());
    target_->SetDpi(Dpi(), Dpi());
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    const D2D1_SIZE_F size = target_->GetSize();
    const Layout layout = ComputeLayout(size.width, size.height);
    DrawBackground(size.width, size.height);
    DrawPreviewFrame(layout.card);
    DrawHeader(layout.header);
    DrawControlStrip(layout.strip);
    DrawSidebar(layout.sidebar);
    const HRESULT hr = target_->EndDraw();
    target_->SetTarget(nullptr);
    if (FAILED(hr)) qbt::Log("ui: draw failed 0x%08lx", hr);
    if (SUCCEEDED(hr)) swapChain_->Present(1, 0);

    if (released_) active_ = 0;
    pressed_ = released_ = false;
    wheel_ = 0;
    if (hot_ != nextHot_) {
        hot_ = nextHot_;
        POINT cursor;
        GetCursorPos(&cursor);
        if (WindowFromPoint(cursor) == window_) SetCursor(LoadCursorW(nullptr, hot_ ? IDC_HAND : IDC_ARROW));
    }
    if (animating_) SetTimer(window_, kAnimationTimer, 16, nullptr);
    else KillTimer(window_, kAnimationTimer);
    // Live dots breathe slowly; no need for 60 fps there.
    if (wantsPulse_ && IsWindowVisible(parent_) && !IsIconic(parent_)) SetTimer(window_, kPulseTimer, 50, nullptr);
    else KillTimer(window_, kPulseTimer);
}

void StudioUi::DrawBackground(float width, float height) {
    // Over Mica: a light wash evens out the wallpaper tint; without Mica: opaque base.
    target_->Clear(backdrop_ ? D2D1::ColorF(0, 0, 0, 0) : palette_.base);
    Fill({0, 0, width, height}, 0, palette_.wash);
    // Soft color fields, like the environment behind visionOS windows.
    const auto blob = [&](float cx, float cy, float rx, float ry, Color color) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, color}, {0.55f, Faded(color, 0.45f)}, {1.0f, Faded(color, 0.0f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1RadialGradientBrush> gradient;
        target_->CreateGradientStopCollection(stops, 3, &collection);
        if (collection)
            target_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), {}, rx, ry),
                                               collection.Get(), &gradient);
        if (gradient) target_->FillRectangle(D2D1::RectF(0, 0, width, height), gradient.Get());
    };
    blob(width * 0.12f, height * 0.05f, width * 0.62f, height * 0.75f, palette_.blobA);
    blob(width * 0.98f, height * 0.95f, width * 0.55f, height * 0.8f, palette_.blobB);
    blob(width * 0.55f, height * 1.08f, width * 0.42f, height * 0.38f, palette_.blobC);
}

void StudioUi::DrawHeader(Rect area) {
    const float cy = (area.top + area.bottom) / 2 + 2;
    Logo(area.left, cy - 17, 34);
    Text(L"QBT Camera", {area.left + 46, cy - 20, area.left + 260, cy + 4}, titleFormat_.Get(), palette_.text);
    Text(L"Studio  ·  iPhone как профессиональная веб-камера", {area.left + 46, cy + 3, area.left + 420, cy + 21},
         smallFormat_.Get(), palette_.secondary);

    // Right side (left of the caption buttons): theme switch, then status pills.
    float right = area.right - kCaptionButtons + kPad;
    const Rect themeButton{right - 36, cy - 18, right, cy + 18};
    headerControls_.push_back(themeButton);
    if (Button(kThemeButton, themeButton)) {
        themeMode_ = themeMode_ == ThemeMode::Auto ? ThemeMode::Light : themeMode_ == ThemeMode::Light ? ThemeMode::Dark
                                                                                                      : ThemeMode::Auto;
        SaveThemeMode();
        ApplyTheme(true);
    }
    Glass(themeButton, 18, false);
    const float themeHover = Animate(kThemeButton + kHoverOffset, Hit(themeButton) ? 1.0f : 0.0f);
    if (themeHover > 0.01f) Fill(themeButton, 18, Faded(palette_.hover, themeHover * 2));
    Icon(themeMode_ == ThemeMode::Auto ? kIconAutoTheme : themeMode_ == ThemeMode::Light ? kIconSun : kIconMoon, themeButton,
         palette_.text, 15);
    right = themeButton.left - 10;

    const auto pill = [&](wchar_t icon, const std::wstring& text, Color color, bool pulse) {
        const float width = Measure(text, strongFormat_.Get()) + 50;
        const Rect rect{right - width, cy - 18, right, cy + 18};
        Glass(rect, 18, false);
        const float x = rect.left + 16;
        if (icon) {
            Icon(icon, {x - 6, cy - 9, x + 14, cy + 9}, color, 15);
        } else {
            if (pulse) {
                wantsPulse_ = true;
                const float breathe = 0.5f + 0.5f * std::sin(time_ * 3.2f);
                Dot(x + 4, cy, 4 + 6 * breathe, Faded(color, 0.35f * (1 - breathe)));
            }
            Dot(x + 4, cy, 4, color);
        }
        Text(text, {x + 18, cy - 10, rect.right - 12, cy + 10}, strongFormat_.Get(), palette_.text);
        right = rect.left - 10;
    };

    if (!status_.cameraInstalled) pill(kIconWarning, L"Камера не установлена", kAmber, false);
    else if (status_.cameraInUse) pill(0, L"Камера в работе", kRed, true);
    else pill(kIconMonitor, L"Камера готова", palette_.secondary, false);

    switch (status_.phase) {
    case HostStatus::Phase::Starting:
        pill(0, L"Запуск…", palette_.tertiary, false);
        break;
    case HostStatus::Phase::Searching:
        pill(0, L"Ищем iPhone", kAmber, true);
        break;
    case HostStatus::Phase::Connected:
        pill(status_.usb ? kIconUsb : kIconWifi, std::wstring(L"В эфире · ") + (status_.usb ? L"USB" : L"Wi-Fi"), kGreen, false);
        break;
    case HostStatus::Phase::Paused:
        pill(0, L"Пауза: приложение свёрнуто", kAmber, true);
        break;
    case HostStatus::Phase::Error:
        pill(0, status_.error, kRed, false);
        break;
    }
}

void StudioUi::DrawPreviewFrame(Rect card) {
    // A floating "screen": deep shadow, dark glass, thin rim. The video visual covers the inside.
    Shadow(card, kCardRadius, 26, 16, Faded(palette_.shadow, 1.3f));
    Fill(card, kCardRadius, dark_ ? Rgb(0x0A0C12) : Rgb(0xEAEFF8));
    Stroke(card, kCardRadius, dark_ ? White(0.12f) : White(0.85f), 1.5f);
}

void StudioUi::DrawControlStrip(Rect area) {
    // Row 1: lens switcher, mirror and torch, current quality.
    const float rowTop = area.top, rowBottom = area.top + 46;
    const bool anyLensKnown = std::any_of(std::begin(lensAvailable_), std::end(lensAvailable_), [](bool b) { return b; });
    const std::wstring lensTitles[4] = {L"0.5×", L"1×", teleTitle_, L"Фронт"};
    float itemWidths[4] = {};
    float itemsWidth = 0;
    for (int i = 0; i < 4; ++i) {
        if (anyLensKnown && !lensAvailable_[i]) continue;
        itemWidths[i] = Measure(lensTitles[i], strongFormat_.Get()) + 32;
        itemsWidth += itemWidths[i];
    }
    const Rect pill{area.left, rowTop, area.left + itemsWidth + 10, rowBottom};
    Glass(pill, 23);
    float x = pill.left + 5;
    int selected = -1;
    float selectedX = 0;
    for (int i = 0; i < 4; ++i) {
        if (itemWidths[i] == 0) continue;
        if (connected_ && lens_ == kLensNames[i]) {
            selected = i;
            selectedX = x;
        }
        x += itemWidths[i];
    }
    if (selected >= 0) {
        const float indicatorX = Animate(kLensIndicatorX, selectedX, 16);
        const float indicatorWidth = Animate(kLensIndicatorWidth, itemWidths[selected], 16);
        Selection({indicatorX, rowTop + 5, indicatorX + indicatorWidth, rowBottom - 5}, 18, 1);
    }
    x = pill.left + 5;
    for (int i = 0; i < 4; ++i) {
        if (itemWidths[i] == 0) continue;
        const Rect item{x, rowTop + 5, x + itemWidths[i], rowBottom - 5};
        const bool enabled = connected_ && lensAvailable_[i];
        if (Button(kLensBase + i, item, enabled) && lens_ != kLensNames[i]) {
            lens_ = kLensNames[i];
            Queue(Group::Lens, Format(R"({"cmd":"lens","value":"%s"})", kLensNames[i]));
            Flush();
        }
        const float hover = Animate(kLensBase + i + kHoverOffset, enabled && Hit(item) && i != selected ? 1.0f : 0.0f);
        if (hover > 0.01f) Fill(item, 18, Faded(palette_.hover, hover * 1.5f));
        Text(lensTitles[i], item, strongFormat_.Get(),
             !enabled ? palette_.tertiary : i == selected ? palette_.selectedText : palette_.secondary,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        x += itemWidths[i];
    }

    // Round glass buttons next to the lenses.
    x = pill.right + 12;
    const auto roundButton = [&](int id, wchar_t icon, bool active, bool enabled) {
        const Rect rect{x, rowTop, x + 46, rowBottom};
        x += 56;
        const bool clicked = Button(id, rect, enabled);
        Glass(rect, 23);
        const float on = Animate(id + kSelectOffset, active ? 1.0f : 0.0f);
        if (on > 0.01f) Fill(Inflate(rect, -4), 19, Faded(palette_.accent, on));
        const float hover = Animate(id + kHoverOffset, enabled && Hit(rect) ? 1.0f : 0.0f);
        if (hover > 0.01f) Fill(rect, 23, Faded(palette_.hover, hover * 1.5f));
        Icon(icon, rect, !enabled ? palette_.tertiary : Mix(palette_.text, White(1), on), 16);
        return clicked;
    };
    if (roundButton(kMirrorButton, kIconMirror, mirror_, connected_)) {
        mirror_ = !mirror_;
        SendEffects();
    }
    const bool torchOn = torch_ > 0.01;
    if (roundButton(kTorchButton, kIconTorch, torchOn, connected_ && hasTorch_ && (torchAvailable_ || torchOn))) {
        torch_ = torchOn ? 0 : 0.5;
        Queue(Group::Torch, Format(R"({"cmd":"torch","value":%.2f})", torch_));
    }
    if (connected_ && hasTorch_ && !torchAvailable_ && !torchOn)
        Text(L"Фонарик недоступен: iPhone нагрелся", {x, rowTop, area.right - 200, rowBottom}, smallFormat_.Get(), kAmber);

    // Quality on the right.
    if (status_.phase == HostStatus::Phase::Connected && status_.height > 0) {
        const std::wstring quality = WideFormat(L"%d × %d", status_.width, status_.height);
        const float width = Measure(quality, strongFormat_.Get()) + 32;
        const Rect badge{area.right - width, rowTop + 6, area.right, rowBottom - 6};
        Glass(badge, 17, false);
        Text(quality, badge, strongFormat_.Get(), palette_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Row 2: telemetry tiles.
    const float tilesTop = rowBottom + 12;
    const float gap = 12;
    const float tileWidth = (area.right - area.left - gap * 3) / 4;
    const auto tile = [&](int index, wchar_t icon, Color tint, const wchar_t* label, const std::wstring& value, Color valueColor) {
        const float left = area.left + index * (tileWidth + gap);
        const Rect rect{left, tilesTop, left + tileWidth, area.bottom};
        Glass(rect, 18);
        const float cy = (rect.top + rect.bottom) / 2;
        const Rect iconBox{left + 12, cy - 16, left + 44, cy + 16};
        Fill(iconBox, 16, Faded(tint, dark_ ? 0.22f : 0.16f));
        Icon(icon, iconBox, tint, 15);
        Text(label, {left + 54, rect.top + 6, rect.right - 8, cy}, smallFormat_.Get(), palette_.secondary,
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
        Text(value, {left + 54, cy - 1, rect.right - 8, rect.bottom - 4}, strongFormat_.Get(), valueColor,
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    };
    const bool live = status_.phase == HostStatus::Phase::Connected;
    tile(0, kIconTimer, kGreen, L"Задержка", roundTripMillis_ >= 0 && live ? WideFormat(L"%.0f мс", roundTripMillis_) : L"—",
         roundTripMillis_ < 80 ? palette_.text : kAmber);
    tile(1, kIconPulse, kCyan, L"Поток", live ? WideFormat(L"%.1f Мбит/с", status_.megabits) : L"—", palette_.text);
    tile(2, kIconVideo, kBlue, L"Кадры", live ? WideFormat(L"%.0f к/с", status_.fps) : L"—", palette_.text);
    std::wstring heat = L"—";
    Color heatColor = palette_.text;
    if (connected_ && thermal_ == "nominal") heat = L"Норма";
    else if (connected_ && thermal_ == "fair") heat = L"Тёплый";
    else if (connected_ && thermal_ == "serious") heat = L"Горячий", heatColor = kAmber;
    else if (connected_ && thermal_ == "critical") heat = L"Перегрев", heatColor = kRed;
    tile(3, kIconThermometer, kAmber, L"Нагрев iPhone", heat, heatColor);
}

void StudioUi::DrawSidebar(Rect area) {
    Glass(area, 26);

    // Tab bar: segmented control with a sliding selection.
    const Rect bar{area.left + 12, area.top + 12, area.right - 12, area.top + 12 + 58};
    Fill(bar, 18, palette_.control);
    const wchar_t icons[4] = {kIconCamera, kIconSparkle, kIconMicrophone, kIconBolt};
    const wchar_t* titles[4] = {L"Камера", L"Эффекты", L"Звук", L"Поток"};
    const float tabWidth = (bar.right - bar.left - 8) / 4;
    const float indicatorX = Animate(kTabIndicatorX, bar.left + 4 + tab_ * tabWidth, 16);
    Selection({indicatorX, bar.top + 4, indicatorX + tabWidth, bar.bottom - 4}, 14, 1);
    for (int i = 0; i < 4; ++i) {
        const Rect item{bar.left + 4 + i * tabWidth, bar.top + 4, bar.left + 4 + (i + 1) * tabWidth, bar.bottom - 4};
        if (Button(kTabBase + i, item) && tab_ != i) {
            tab_ = i;
            scroll_ = scrollTarget_ = 0;
            animations_[kScroll] = 0;
        }
        const float hover = Animate(kTabBase + i + kHoverOffset, Hit(item) && i != tab_ ? 1.0f : 0.0f);
        if (hover > 0.01f) Fill(item, 14, Faded(palette_.hover, hover));
        const Color color = i == tab_ ? palette_.selectedText : Mix(palette_.secondary, palette_.text, hover);
        Icon(icons[i], {item.left, item.top + 6, item.right, item.top + 27}, i == tab_ ? palette_.accent : color, 16);
        Text(titles[i], {item.left, item.top + 28, item.right, item.bottom - 3}, captionFormat_.Get(), color,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Scrollable grouped settings.
    const Rect viewport{area.left + 14, bar.bottom + 6, area.right - 14, area.bottom - 12};
    const float viewportHeight = viewport.bottom - viewport.top;
    if (wheel_ != 0 && Hit(viewport)) scrollTarget_ -= wheel_ / WHEEL_DELTA * 72;
    scrollTarget_ = std::clamp(scrollTarget_, 0.0f, std::max(0.0f, contentHeight_ - viewportHeight));
    scroll_ = Animate(kScroll, scrollTarget_, 18);

    target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    clip_ = viewport;
    const float top = viewport.top + 8 - scroll_;
    const float width = viewport.right - viewport.left;
    float bottom = top;
    switch (tab_) {
    case 0: bottom = CameraTab(viewport.left, top, width); break;
    case 1: bottom = EffectsTab(viewport.left, top, width); break;
    case 2: bottom = AudioTab(viewport.left, top, width); break;
    default: bottom = StreamTab(viewport.left, top, width); break;
    }
    contentHeight_ = bottom - top + 16;
    clip_ = {-1e6f, -1e6f, 1e6f, 1e6f};
    target_->PopAxisAlignedClip();

    if (contentHeight_ > viewportHeight + 1) { // slim scroll indicator
        const float thumbHeight = std::max(36.0f, viewportHeight * viewportHeight / contentHeight_);
        const float travel = viewportHeight - thumbHeight;
        const float thumbTop = viewport.top + travel * (scroll_ / std::max(1.0f, contentHeight_ - viewportHeight));
        Fill({area.right - 7, thumbTop, area.right - 4, thumbTop + thumbHeight}, 1.5f, palette_.track);
    }
}

// ---------------------------------------------------------------------------------------------
// Tabs: grouped settings like iOS / visionOS (caption above, rows inside a rounded platter)

float StudioUi::CameraTab(float x, float y, float width) {
    const float ix = x + kRowPad, iw = width - kRowPad * 2;
    bool changed = false, toggled = false;

    {
        const Rect group = Platter(x, y, width, kSwitchRow + kSliderRow * 3, L"Экспозиция");
        float row = SwitchRow(kExposureAuto, x, group.top, width, L"Автоматически", exposureAuto_, connected_ && manualExposure_, toggled);
        if (toggled) SendExposure();
        Separator(ix, row, iw + kRowPad);
        row = SliderRow(kIso, ix, row, iw, L"ISO", WideFormat(L"%.0f", IsoAt(iso_)), iso_, connected_ && manualExposure_, changed);
        if (changed) {
            exposureAuto_ = false;
            SendExposure();
        }
        row = SliderRow(kShutter, ix, row, iw, L"Выдержка", WideFormat(L"1/%.0f", 1'000'000.0 / ShutterAt(shutter_)), shutter_,
                        connected_ && manualExposure_, changed);
        if (changed) {
            exposureAuto_ = false;
            SendExposure();
        }
        const double ev = std::round((minBias_ + (maxBias_ - minBias_) * bias_) * 10) / 10;
        SliderRow(kBias, ix, row, iw, L"Яркость", WideFormat(L"%+.1f EV", ev), bias_, connected_ && exposureAuto_, changed);
        if (changed) Queue(Group::Bias, Format(R"({"cmd":"exposureBias","value":%.1f})", ev));
        y = group.bottom + kGroupGap;
    }
    {
        const Rect group = Platter(x, y, width, kSwitchRow + kSliderRow * 2, L"Баланс белого");
        float row = SwitchRow(kWhiteBalanceAuto, x, group.top, width, L"Автоматически", whiteBalanceAuto_,
                              connected_ && manualWhiteBalance_, toggled);
        if (toggled) SendWhiteBalance();
        Separator(ix, row, iw + kRowPad);
        const Color cold = Rgb(0x60A5FA), warm = Rgb(0xFBBF24), green = Rgb(0x4ADE80), magenta = Rgb(0xE879F9);
        const double kelvin = std::round((kMinKelvin + (kMaxKelvin - kMinKelvin) * kelvin_) / 100) * 100;
        row = SliderRow(kKelvin, ix, row, iw, L"Температура", WideFormat(L"%.0f K", kelvin), kelvin_,
                        connected_ && manualWhiteBalance_, changed, &cold, &warm);
        if (changed) {
            whiteBalanceAuto_ = false;
            SendWhiteBalance();
        }
        SliderRow(kTint, ix, row, iw, L"Оттенок", WideFormat(L"%+.0f", tint_ * 200 - 100), tint_, connected_ && manualWhiteBalance_,
                  changed, &green, &magenta);
        if (changed) {
            whiteBalanceAuto_ = false;
            SendWhiteBalance();
        }
        y = group.bottom + kGroupGap;
    }
    {
        const Rect group = Platter(x, y, width, kSwitchRow + kSliderRow, L"Фокус");
        float row = SwitchRow(kFocusAuto, x, group.top, width, L"Автофокус", focusAuto_, connected_ && manualFocus_, toggled);
        if (toggled) SendFocus();
        Separator(ix, row, iw + kRowPad);
        SliderRow(kFocus, ix, row, iw, L"Дистанция", WideFormat(L"%.0f %%", focus_ * 100), focus_, connected_ && manualFocus_, changed);
        if (changed) {
            focusAuto_ = false;
            SendFocus();
        }
        y = group.bottom + kGroupGap;
    }
    {
        const bool torchBlocked = connected_ && hasTorch_ && !torchAvailable_ && torch_ < 0.01;
        const Rect group = Platter(x, y, width, kSliderRow * 2 + (torchBlocked ? 24 : 0), L"Зум и фонарик");
        float row = SliderRow(kZoom, ix, group.top, iw, L"Увеличение", WideFormat(L"%.1f×", ZoomAt(zoom_)), zoom_, connected_, changed);
        if (changed) Queue(Group::Zoom, Format(R"({"cmd":"zoom","value":%.3f})", ZoomAt(zoom_)));
        row = SliderRow(kTorch, ix, row, iw, L"Фонарик", torch_ < 0.01 ? std::wstring(L"выкл") : WideFormat(L"%.0f %%", torch_ * 100),
                        torch_, connected_ && hasTorch_ && !torchBlocked, changed);
        if (changed) Queue(Group::Torch, Format(R"({"cmd":"torch","value":%.2f})", torch_));
        if (torchBlocked)
            Text(L"iPhone нагрелся — iOS временно не даёт включить фонарик", {ix, row - 6, ix + iw, row + 16}, smallFormat_.Get(),
                 kAmber);
        y = group.bottom;
    }
    return y;
}

float StudioUi::EffectsTab(float x, float y, float width) {
    const float ix = x + kRowPad, iw = width - kRowPad * 2;
    bool changed = false, toggled = false;

    {
        // Background: mode, then what that mode needs.
        const bool portrait = background_ == "portrait", replace = background_ == "replace", chroma = background_ == "chroma";
        const float chips = 14 + kChipHeight + 14;
        const float backdrops = kChipHeight * 2 + 8 + 14;
        float height = chips;
        if (background_ != "off") height += kChipHeight + 14 + 14;
        if (portrait) height += kSliderRow + 26;
        if (replace) height += backdrops + 26;
        if (chroma) height += backdrops + kSliderRow * 3 + kChipHeight + 14;
        const Rect group = Platter(x, y, width, height, L"Фон");
        float row = group.top + 14;
        const float quarterWidth = (iw - 8 * 3) / 4;
        for (int i = 0; i < 4; ++i) {
            const float cx = ix + i * (quarterWidth + 8);
            if (Chip(kBackgroundBase + i, {cx, row, cx + quarterWidth, row + kChipHeight}, kBackgroundTitles[i],
                     background_ == kBackgroundNames[i], connected_)) {
                background_ = kBackgroundNames[i];
                SendEffects();
            }
        }
        row += kChipHeight + 14;
        if (background_ != "off") {
            // Where the work happens: the PC's GPU (cool phone, full-resolution edges) or the iPhone.
            Separator(ix, row, iw + kRowPad);
            row += 14;
            const wchar_t* titles[2] = {L"Считает компьютер", L"Считает iPhone"};
            const char* names[2] = {"pc", "phone"};
            const float halfWidth = (iw - 8) / 2;
            for (int i = 0; i < 2; ++i) {
                const float cx = ix + i * (halfWidth + 8);
                if (Chip(kProcessingBase + i, {cx, row, cx + halfWidth, row + kChipHeight}, titles[i], processing_ == names[i],
                         connected_)) {
                    processing_ = names[i];
                    SendEffects();
                }
            }
            row += kChipHeight + 14;
        }
        if (portrait || replace) {
            // Where the depth comes from: real sensors make the blur follow the distance like a lens.
            std::wstring source = L"Глубина: по силуэту (нейросеть)";
            if (depthActive_ && depthSensor_ == "lidar") source = L"Глубина: LiDAR + нейросеть";
            else if (depthActive_ && depthSensor_ == "truedepth") source = L"Глубина: TrueDepth + нейросеть";
            Separator(ix, row, iw + kRowPad);
            Text(source, {ix, row, ix + iw, row + 26}, smallFormat_.Get(), depthActive_ ? kGreen : palette_.tertiary);
            row += 26;
        }
        if (portrait) {
            SliderRow(kBlur, ix, row, iw, L"Размытие", WideFormat(L"%.0f %%", blur_ * 100), blur_, connected_, changed);
            if (changed) SendEffects();
        }
        if (replace || chroma) {
            if (chroma) Separator(ix, row, iw + kRowPad);
            const float thirdWidth = (iw - 16) / 3;
            float top = row + (chroma ? 14 : 0);
            for (int i = 0; i < 6; ++i) {
                const float cx = ix + (i % 3) * (thirdWidth + 8);
                const float cy = top + (i / 3) * (kChipHeight + 8);
                if (Chip(kBackdropBase + i, {cx, cy, cx + thirdWidth, cy + kChipHeight}, kBackdropTitles[i],
                         backdropName_ == kBackdropNames[i], connected_)) {
                    backdropName_ = kBackdropNames[i];
                    SendEffects();
                }
            }
            row = top + kChipHeight * 2 + 8 + 14;
        }
        if (chroma) {
            Separator(ix, row, iw + kRowPad);
            row = SliderRow(kKeySensitivity, ix, row, iw, L"Чувствительность", WideFormat(L"%.0f %%", keySensitivity_ * 100),
                            keySensitivity_, connected_, changed);
            if (changed) SendEffects();
            row = SliderRow(kKeySoftness, ix, row, iw, L"Мягкость края", WideFormat(L"%.0f %%", keySoftness_ * 100), keySoftness_,
                            connected_, changed);
            if (changed) SendEffects();
            row = SliderRow(kKeySpill, ix, row, iw, L"Убрать зелёный отсвет", WideFormat(L"%.0f %%", keySpill_ * 100), keySpill_,
                            connected_, changed);
            if (changed) SendEffects();
            if (Chip(kKeyResample, {ix, row + 4, ix + iw, row + 4 + kChipHeight}, L"Определить цвет фона заново", false, connected_))
                Queue(Group::Effects, R"({"cmd":"effects","keyResample":true})");
        }
        y = group.bottom + kGroupGap;
    }
    {
        const Rect group = Platter(x, y, width, kSliderRow + kSwitchRow, L"Лицо");
        float row = SliderRow(kRetouch, ix, group.top, iw, L"Ретушь кожи",
                              retouch_ < 0.01 ? std::wstring(L"выкл") : WideFormat(L"%.0f %%", retouch_ * 100), retouch_, connected_,
                              changed);
        if (changed) SendEffects();
        Separator(ix, row, iw + kRowPad);
        SwitchRow(kFraming, x, row, width, L"Следить за лицом", framing_, connected_, toggled);
        if (toggled) SendEffects();
        y = group.bottom + kGroupGap;
    }
    {
        const float chips = kChipHeight * 2 + 8;
        const Rect group = Platter(x, y, width, 14 + chips + 6 + kSliderRow, L"Цветовой фильтр");
        float row = group.top + 14;
        const float chipWidth = (iw - 8 * 3) / 4;
        for (int i = 0; i < 8; ++i) {
            const float cx = ix + (i % 4) * (chipWidth + 8);
            const float cy = row + (i / 4) * (kChipHeight + 8);
            if (Chip(kFilterBase + i, {cx, cy, cx + chipWidth, cy + kChipHeight}, kFilterTitles[i], filter_ == kFilterNames[i],
                     connected_)) {
                filter_ = kFilterNames[i];
                SendEffects();
            }
        }
        row += chips + 6;
        SliderRow(kFilterAmount, ix, row, iw, L"Сила", WideFormat(L"%.0f %%", filterAmount_ * 100), filterAmount_,
                  connected_ && filter_ != "none", changed);
        if (changed) SendEffects();
        y = group.bottom + kGroupGap;
    }
    {
        const Rect group = Platter(x, y, width, kSwitchRow, L"Кадр");
        SwitchRow(kMirrorSwitch, x, group.top, width, L"Отразить по горизонтали", mirror_, connected_, toggled);
        if (toggled) SendEffects();
        y = group.bottom + 12;
    }

    std::wstring info;
    if (!connected_) info = L"Подключите iPhone, чтобы настроить эффекты.";
    else if (effectsMillis_ > 0.05)
        info = WideFormat(L"Эффекты считает iPhone: %.1f мс на кадр (нейросеть %.1f мс, остальное — видеокарта).", effectsMillis_,
                          visionMillis_);
    else info = L"Эффекты выключены: видео идёт без обработки.";
    return InfoText(x + 6, y, width - 12, info, palette_.tertiary);
}

float StudioUi::AudioTab(float x, float y, float width) {
    const float ix = x + kRowPad, iw = width - kRowPad * 2;
    bool toggled = false;

    {
        const Rect group = Platter(x, y, width, kSwitchRow + 40, L"Микрофон iPhone");
        float row = SwitchRow(kMicrophone, x, group.top, width, L"Передавать звук", microphoneOn_, connected_ && microphoneAvailable_,
                              toggled);
        if (toggled) Queue(Group::Audio, Format(R"({"cmd":"audio","enabled":%s})", microphoneOn_ ? "true" : "false"));
        Separator(ix, row, iw + kRowPad);
        // Level meter: rounded segments, green → amber → red.
        constexpr int kBars = 30;
        const float gap = 3;
        const float barWidth = (iw - gap * (kBars - 1)) / kBars;
        const float lit = microphoneOn_ ? level_ * kBars : 0;
        for (int i = 0; i < kBars; ++i) {
            const float position = static_cast<float>(i) / kBars;
            const Color color = position < 0.68f ? kGreen : position < 0.88f ? kAmber : kRed;
            const float amount = std::clamp(lit - i, 0.0f, 1.0f);
            const Rect bar{ix + i * (barWidth + gap), row + 12, ix + i * (barWidth + gap) + barWidth, row + 28};
            Fill(bar, 2.5f, palette_.track);
            if (amount > 0) Fill(bar, 2.5f, Faded(color, 0.3f + 0.7f * amount));
        }
        y = group.bottom + kGroupGap;
    }
    {
        const float rows = static_cast<float>(devices_.size() + 1);
        const Rect group = Platter(x, y, width, rows * 44, L"Где играть звук на ПК");
        float row = group.top;
        for (size_t i = 0; i <= devices_.size(); ++i) {
            const std::wstring id = i == 0 ? std::wstring() : devices_[i - 1].first;
            const std::wstring name = i == 0 ? std::wstring(L"Не выводить") : devices_[i - 1].second;
            const bool selected = id == selectedDevice_;
            const int widgetId = kDeviceBase + static_cast<int>(i);
            const Rect item{x + 4, row + 3, x + width - 4, row + 41};
            if (Button(widgetId, item) && !selected) {
                selectedDevice_ = id;
                if (deviceSink_) deviceSink_(id);
            }
            const float hover = Animate(widgetId + kHoverOffset, Hit(item) ? 1.0f : 0.0f);
            const float pick = Animate(widgetId + kSelectOffset, selected ? 1.0f : 0.0f);
            if (hover > 0.01f) Fill(item, 11, Faded(palette_.hover, hover * 1.5f));
            const float cy = row + 22;
            if (i > 0) Separator(ix + 30, row, iw - 30 + kRowPad);
            brush_->SetColor(Mix(palette_.track, palette_.accent, pick));
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ix + 9, cy), 8.5f, 8.5f), brush_.Get(), 1.6f);
            if (pick > 0.01f) Dot(ix + 9, cy, 5 * pick, palette_.accent);
            Text(name, {ix + 30, row, ix + iw, row + 44}, selected ? strongFormat_.Get() : bodyFormat_.Get(),
                 selected ? palette_.text : palette_.secondary);
            row += 44;
        }
        y = group.bottom + 12;
    }

    const auto isCable = [](const std::wstring& name) { return name.find(L"CABLE Input") != std::wstring::npos; };
    const auto selected = std::find_if(devices_.begin(), devices_.end(),
                                       [&](const AudioDevice& d) { return d.first == selectedDevice_; });
    std::wstring info;
    Color color = palette_.tertiary;
    if (microphoneDenied_) {
        info = L"iPhone не дал доступ к микрофону: Настройки → QBT Camera → Микрофон.";
        color = kAmber;
    } else if (selected != devices_.end() && isCable(selected->second)) {
        info = L"В Zoom, Discord или Teams выберите микрофон «CABLE Output».";
    } else if (selected == devices_.end()) {
        info = L"Звук iPhone принимается, но никуда не выводится. Свой микрофон «QBT Camera» для звонков появится "
               L"в одной из следующих версий.";
    } else {
        info = L"Звук играет в «" + selected->second + L"» — это прослушка, не микрофон для звонков.";
    }
    return InfoText(x + 6, y, width - 12, info, color);
}

float StudioUi::StreamTab(float x, float y, float width) {
    const float ix = x + kRowPad, iw = width - kRowPad * 2;
    bool changed = false, toggled = false;

    {
        const Rect group = Platter(x, y, width, 14 + kChipHeight * 2 + 8 + 14, L"Качество");
        float row = group.top + 14;
        const float third = (iw - 16) / 3;
        for (int i = 0; i < 3; ++i) {
            const float cx = ix + i * (third + 8);
            if (Chip(kResolutionBase + i, {cx, row, cx + third, row + kChipHeight}, kResolutionTitles[i],
                     resolution_ == kResolutionNames[i], connected_)) {
                resolution_ = kResolutionNames[i];
                SendPreset();
            }
        }
        row += kChipHeight + 8;
        const float half = (iw - 8) / 2;
        for (int i = 0; i < 2; ++i) {
            const float cx = ix + i * (half + 8);
            if (Chip(kFpsBase + i, {cx, row, cx + half, row + kChipHeight}, WideFormat(L"%d к/с", kFrameRates[i]),
                     fps_ == kFrameRates[i], connected_)) {
                fps_ = kFrameRates[i];
                SendPreset();
            }
        }
        y = group.bottom + kGroupGap;
    }
    {
        const Rect group = Platter(x, y, width, 14 + kChipHeight * 2 + 8 + 14, L"Ориентация");
        const float row = group.top + 14;
        const float half = (iw - 8) / 2;
        for (int i = 0; i < 4; ++i) {
            const float cx = ix + (i % 2) * (half + 8);
            const float cy = row + (i / 2) * (kChipHeight + 8);
            if (Chip(kOrientationBase + i, {cx, cy, cx + half, cy + kChipHeight}, kOrientationTitles[i],
                     orientation_ == kOrientationNames[i], connected_)) {
                orientation_ = kOrientationNames[i];
                Queue(Group::Stream, Format(R"({"cmd":"orientation","value":"%s"})", kOrientationNames[i]));
            }
        }
        y = group.bottom + kGroupGap;
    }
    {
        const Rect group = Platter(x, y, width, kHintRow, L"Экран iPhone");
        SwitchRow(kBlackout, x, group.top, width, L"Чёрный экран", blackout_, connected_, toggled,
                  L"Экономит батарею и снижает нагрев");
        if (toggled) Queue(Group::Screen, Format(R"({"cmd":"screen","blackout":%s})", blackout_ ? "true" : "false"));
        y = group.bottom + kGroupGap;
    }
    {
        const Rect group = Platter(x, y, width, kSwitchRow * 2 + kSliderRow, L"Суфлёр у объектива");
        float row = SwitchRow(kPrompterVisible, x, group.top, width, L"Показать на iPhone", prompterVisible_, connected_, toggled);
        if (toggled) SendPrompter();
        Separator(ix, row, iw + kRowPad);
        row = SwitchRow(kPrompterPlaying, x, row, width, L"Прокрутка", prompterPlaying_, connected_, toggled);
        if (toggled) SendPrompter();
        Separator(ix, row, iw + kRowPad);
        SliderRow(kPrompterSpeed, ix, row, iw, L"Скорость", WideFormat(L"%.0f из 5", 1 + prompterSpeed_ * 4), prompterSpeed_,
                  connected_, changed);
        if (changed) {
            prompterSpeed_ = std::round(prompterSpeed_ * 4) / 4;
            SendPrompter();
        }
        y = group.bottom + kGroupGap;
    }
    {
        const std::wstring camera = !status_.cameraInstalled ? L"Камера «QBT Camera» не установлена: запустите install_camera.ps1."
                                    : status_.cameraInUse    ? L"Камера «QBT Camera» сейчас используется приложением."
                                                             : L"Камера «QBT Camera» видна во всех приложениях: Zoom, Teams, OBS, браузеры.";
        const float info = TextHeight(camera, iw);
        const Rect group = Platter(x, y, width, kSwitchRow + info + 14, L"Компьютер");
        bool autostart = shell::IsAutostartEnabled();
        const float row = SwitchRow(kAutostart, x, group.top, width, L"Запускать вместе с Windows", autostart, true, toggled);
        if (toggled) shell::SetAutostart(autostart);
        Separator(ix, row, iw + kRowPad);
        InfoText(ix, row + 7, iw, camera, palette_.tertiary);
        y = group.bottom;
    }
    return y;
}

// ---------------------------------------------------------------------------------------------
// Drawing primitives

void StudioUi::Fill(const Rect& rect, float radius, Color color) {
    if (color.a <= 0.001f) return;
    brush_->SetColor(color);
    if (radius <= 0) target_->FillRectangle(rect, brush_.Get());
    else target_->FillRoundedRectangle({rect, radius, radius}, brush_.Get());
}

void StudioUi::Stroke(const Rect& rect, float radius, Color color, float width) {
    if (color.a <= 0.001f) return;
    brush_->SetColor(color);
    target_->DrawRoundedRectangle({Inflate(rect, -width / 2), radius, radius}, brush_.Get(), width);
}

void StudioUi::Shadow(const Rect& rect, float radius, float blur, float offsetY, Color color) {
    if (color.a <= 0.001f) return;
    // The shape is recorded once per size; the shadow effect blurs it on the GPU.
    const auto key = std::make_tuple(static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right),
                                     static_cast<int>(rect.bottom), static_cast<int>(radius * 10 + blur * 1000));
    auto& shape = shadowShapes_[key];
    if (!shape) {
        if (shadowShapes_.size() > 96) { // layout changed a lot: start over
            shadowShapes_.clear();
            return;
        }
        ComPtr<ID2D1Image> previous;
        target_->GetTarget(&previous);
        if (FAILED(target_->CreateCommandList(&shape))) return;
        target_->SetTarget(shape.Get());
        brush_->SetColor(Black(1));
        target_->FillRoundedRectangle({rect, radius, radius}, brush_.Get());
        target_->SetTarget(previous.Get());
        shape->Close();
    }
    ComPtr<ID2D1Effect> effect;
    if (FAILED(target_->CreateEffect(CLSID_D2D1Shadow, &effect))) return;
    effect->SetInput(0, shape.Get());
    effect->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur / 2);
    effect->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(color.r, color.g, color.b, color.a));
    target_->DrawImage(effect.Get(), D2D1::Point2F(0, offsetY));
}

void StudioUi::Glass(const Rect& rect, float radius, bool elevated) {
    if (elevated) Shadow(rect, radius, 22, 10, palette_.shadow);
    Fill(rect, radius, palette_.glassFill);
    // Light falls from above: a sheen across the top half and a bright rim along the top edge.
    sheenBrush_->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
    sheenBrush_->SetEndPoint(D2D1::Point2F(rect.left, rect.top + std::min(140.0f, rect.bottom - rect.top)));
    sheenBrush_->SetOpacity(palette_.glassSheen.a);
    target_->FillRoundedRectangle({rect, radius, radius}, sheenBrush_.Get());
    Stroke(rect, radius, palette_.glassBorder);
    rimBrush_->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
    rimBrush_->SetEndPoint(D2D1::Point2F(rect.left, rect.top + std::min(36.0f, (rect.bottom - rect.top) * 0.6f)));
    rimBrush_->SetOpacity(palette_.glassRim.a);
    target_->DrawRoundedRectangle({Inflate(rect, -0.6f), radius, radius}, rimBrush_.Get(), 1.2f);
}

StudioUi::Rect StudioUi::Platter(float x, float y, float width, float height, const wchar_t* title) {
    if (title) {
        Text(title, {x + 6, y, x + width, y + kGroupTitle - 6}, groupFormat_.Get(), palette_.secondary);
        y += kGroupTitle;
    }
    const Rect rect{x, y, x + width, y + height};
    Fill(rect, 16, palette_.platter);
    Stroke(rect, 16, palette_.platterBorder);
    return rect;
}

void StudioUi::Separator(float x, float y, float width) {
    Fill({x, y - 0.5f, x + width, y + 0.5f}, 0, palette_.separator);
}

void StudioUi::Selection(const Rect& rect, float radius, float amount) {
    if (amount <= 0.01f) return;
    if (!dark_) Shadow(rect, radius, 8, 2, Faded(palette_.shadow, amount));
    Fill(rect, radius, Faded(palette_.selectedFill, amount));
    rimBrush_->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
    rimBrush_->SetEndPoint(D2D1::Point2F(rect.left, rect.bottom));
    rimBrush_->SetOpacity((dark_ ? 0.35f : 0.9f) * amount);
    target_->DrawRoundedRectangle({Inflate(rect, -0.5f), radius, radius}, rimBrush_.Get(), 1);
}

void StudioUi::Text(const std::wstring& text, const Rect& rect, IDWriteTextFormat* format, Color color,
                    DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT vertical) {
    if (text.empty() || !format) return;
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(vertical);
    brush_->SetColor(color);
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void StudioUi::Icon(wchar_t glyph, const Rect& rect, Color color, float size) {
    auto& format = iconFormats_[static_cast<int>(size * 10)];
    if (!format) {
        dwrite_->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
        if (!format) return;
    }
    Text(std::wstring(1, glyph), rect, format.Get(), color, DWRITE_TEXT_ALIGNMENT_CENTER);
}

float StudioUi::Measure(const std::wstring& text, IDWriteTextFormat* format) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 10000, 100, &layout))) return 0;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

float StudioUi::TextHeight(const std::wstring& text, float width) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), wrapFormat_.Get(), width, 1000, &layout)))
        return 0;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.height;
}

void StudioUi::Logo(float x, float y, float size) {
    // QBT logo from its 100x100 viewBox: rounded-square outline with a corner badge, softly glowing.
    const auto at = [size](float units) { return units * size / 100.0f; };
    const D2D1_ROUNDED_RECT outline{D2D1::RectF(x + at(11), y + at(11), x + at(83), y + at(83)), at(24), at(24)};
    brush_->SetColor(Rgb(0x2B7FFF, dark_ ? 0.22f : 0.14f));
    target_->DrawRoundedRectangle(outline, brush_.Get(), at(24));
    brush_->SetColor(Rgb(0x2B7FFF));
    target_->DrawRoundedRectangle(outline, brush_.Get(), at(10.5f));
    brush_->SetColor(Rgb(0x539BFF));
    target_->FillRoundedRectangle({D2D1::RectF(x + at(66), y + at(66), x + at(94), y + at(94)), at(9), at(9)}, brush_.Get());
}

void StudioUi::Dot(float cx, float cy, float radius, Color color) {
    brush_->SetColor(color);
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_.Get());
}

// ---------------------------------------------------------------------------------------------
// Widgets

float StudioUi::Animate(int id, float target, float speed) {
    auto [it, inserted] = animations_.try_emplace(id, target);
    float& value = it->second;
    if (inserted) return value;
    value += (target - value) * (1.0f - std::exp(-speed * deltaSeconds_));
    if (std::abs(target - value) < 0.002f * std::max(1.0f, std::abs(target))) value = target;
    else animating_ = true;
    return value;
}

bool StudioUi::Hit(const Rect& rect) const {
    const auto inside = [this](const Rect& r) {
        return mouse_.x >= r.left && mouse_.x < r.right && mouse_.y >= r.top && mouse_.y < r.bottom;
    };
    return inside(rect) && inside(clip_);
}

bool StudioUi::Button(int id, const Rect& rect, bool enabled) {
    if (!enabled) return false;
    const bool hover = Hit(rect);
    if (hover && (active_ == 0 || active_ == id)) nextHot_ = id;
    if (hover && pressed_) active_ = id;
    return released_ && active_ == id && hover;
}

bool StudioUi::Chip(int id, const Rect& rect, const std::wstring& text, bool selected, bool enabled) {
    const bool clicked = Button(id, rect, enabled);
    const float radius = 12;
    const float pick = Animate(id + kSelectOffset, selected ? 1.0f : 0.0f);
    const float hover = Animate(id + kHoverOffset, enabled && Hit(rect) && !selected ? 1.0f : 0.0f);
    Fill(rect, radius, palette_.control);
    if (hover > 0.01f) Fill(rect, radius, Faded(palette_.hover, hover * 1.5f));
    if (pick > 0.01f) {
        Shadow(rect, radius, 10, 3, Faded(palette_.accent, 0.35f * pick));
        Fill(rect, radius, Faded(palette_.accent, pick));
    }
    const Color color = !enabled ? palette_.tertiary : Mix(Mix(palette_.secondary, palette_.text, hover), White(1), pick);
    Text(text, rect, selected ? strongFormat_.Get() : bodyFormat_.Get(), color, DWRITE_TEXT_ALIGNMENT_CENTER);
    return clicked;
}

bool StudioUi::Switch(int id, float right, float centerY, bool& value, bool enabled) {
    const Rect rect{right - 46, centerY - 13, right, centerY + 13};
    const bool clicked = Button(id, Inflate(rect, 4), enabled);
    if (clicked) value = !value;
    const float on = Animate(id + kSwitchOffset, value ? 1.0f : 0.0f, 18);
    const float alpha = enabled ? 1.0f : 0.45f;
    Fill(rect, 13, Faded(Mix(palette_.track, palette_.accent, on), alpha));
    const float knobX = rect.left + 13 + on * (rect.right - rect.left - 26);
    Dot(knobX, centerY + 1, 11, Black(0.16f * alpha));
    Dot(knobX, centerY, 10.5f, White(enabled ? 1.0f : 0.8f));
    return clicked;
}

bool StudioUi::Slider(int id, const Rect& rect, double& value01, bool enabled, const Color* gradientFrom, const Color* gradientTo) {
    const Rect hitRect{rect.left - 10, rect.top - 6, rect.right + 10, rect.bottom + 6};
    bool changed = false;
    if (enabled) {
        const bool hover = Hit(hitRect);
        if (hover && (active_ == 0 || active_ == id)) nextHot_ = id;
        if (hover && pressed_) active_ = id;
        if (active_ == id && (mouseDown_ || released_)) {
            const double value = std::clamp<double>((mouse_.x - rect.left) / std::max(1.0f, rect.right - rect.left), 0.0, 1.0);
            if (std::abs(value - value01) > 1e-4) {
                value01 = value;
                changed = true;
            }
        }
    }
    const float alpha = enabled ? 1.0f : 0.4f;
    const float cy = (rect.top + rect.bottom) / 2;
    const Rect track{rect.left, cy - 3, rect.right, cy + 3};
    const float knobX = rect.left + static_cast<float>(value01) * (rect.right - rect.left);
    if (gradientFrom && gradientTo) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, Faded(*gradientFrom, alpha)},
                                            {0.5f, Faded(dark_ ? White(0.9f) : Rgb(0xE5E7EB), alpha)},
                                            {1.0f, Faded(*gradientTo, alpha)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        target_->CreateGradientStopCollection(stops, 3, &collection);
        if (collection)
            target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, cy), D2D1::Point2F(rect.right, cy)), collection.Get(),
                &gradient);
        if (gradient) target_->FillRoundedRectangle({track, 3, 3}, gradient.Get());
    } else {
        Fill(track, 3, palette_.track);
        if (knobX > track.left + 1) Fill({track.left, track.top, knobX, track.bottom}, 3, Faded(palette_.accent, alpha));
    }
    const float grow = Animate(id + kHoverOffset, enabled && (active_ == id || Hit(hitRect)) ? 1.0f : 0.0f, 20);
    const float radius = 10 + 1.5f * grow;
    if (active_ == id) Dot(knobX, cy, radius + 7, Faded(palette_.accent, 0.18f));
    Dot(knobX, cy + 1.5f, radius + 0.5f, Black(0.18f * alpha));
    Dot(knobX, cy, radius, White(enabled ? 1.0f : 0.85f));
    brush_->SetColor(Black(0.06f));
    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), radius, radius), brush_.Get(), 0.8f);
    return changed;
}

float StudioUi::SliderRow(int id, float x, float y, float width, const wchar_t* label, const std::wstring& value, double& value01,
                          bool enabled, bool& changed, const Color* from, const Color* to) {
    Text(label, {x, y + 8, x + width - 100, y + 28}, bodyFormat_.Get(), enabled ? palette_.text : palette_.tertiary);
    Text(value, {x + width - 130, y + 8, x + width, y + 28}, strongFormat_.Get(), enabled ? palette_.secondary : palette_.tertiary,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    changed = Slider(id, {x + 10, y + 32, x + width - 10, y + 50}, value01, enabled, from, to);
    return y + kSliderRow;
}

float StudioUi::SwitchRow(int id, float x, float y, float width, const wchar_t* label, bool& value, bool enabled, bool& toggled,
                          const wchar_t* hint) {
    const float height = hint ? kHintRow : kSwitchRow;
    const float cy = y + height / 2;
    const Color color = enabled ? palette_.text : palette_.tertiary;
    if (hint) {
        Text(label, {x + kRowPad, cy - 19, x + width - 70, cy + 1}, bodyFormat_.Get(), color);
        Text(hint, {x + kRowPad, cy + 1, x + width - 70, cy + 19}, smallFormat_.Get(), palette_.tertiary);
    } else {
        Text(label, {x + kRowPad, y, x + width - 70, y + height}, bodyFormat_.Get(), color);
    }
    toggled = Switch(id, x + width - kRowPad, cy, value, enabled);
    return y + height;
}

float StudioUi::InfoText(float x, float y, float width, const std::wstring& text, Color color) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), wrapFormat_.Get(), width, 1000, &layout)))
        return y;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    brush_->SetColor(color);
    target_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush_.Get());
    return y + metrics.height;
}

// ---------------------------------------------------------------------------------------------
// Phone state and commands

double StudioUi::IsoAt(double t) const { return minIso_ * std::pow(maxIso_ / minIso_, t); }
double StudioUi::IsoT(double iso) const {
    if (iso <= minIso_ || maxIso_ <= minIso_) return 0;
    return std::clamp(std::log(iso / minIso_) / std::log(maxIso_ / minIso_), 0.0, 1.0);
}
double StudioUi::ZoomAt(double t) const { return minZoom_ * std::pow(maxZoom_ / minZoom_, t); }
double StudioUi::ZoomT(double zoom) const {
    if (zoom <= minZoom_ || maxZoom_ <= minZoom_) return 0;
    return std::clamp(std::log(zoom / minZoom_) / std::log(maxZoom_ / minZoom_), 0.0, 1.0);
}
double StudioUi::ShutterAt(double t) const {
    if (shutterStops_.empty()) return 10000;
    const size_t index = static_cast<size_t>(std::lround(t * (shutterStops_.size() - 1)));
    return shutterStops_[std::min(index, shutterStops_.size() - 1)];
}
double StudioUi::ShutterT(double micros) const {
    if (shutterStops_.size() < 2 || micros <= 0) return 0;
    size_t best = 0;
    for (size_t i = 1; i < shutterStops_.size(); ++i) {
        if (std::abs(std::log(shutterStops_[i] / micros)) < std::abs(std::log(shutterStops_[best] / micros))) best = i;
    }
    return static_cast<double>(best) / (shutterStops_.size() - 1);
}

void StudioUi::OnCapabilities(const JsonObject& caps) {
    connected_ = true;
    lens_ = caps.String("lens");
    const std::string lenses = "," + caps.String("lenses") + ",";
    for (int i = 0; i < 4; ++i) lensAvailable_[i] = lenses.find(std::string(",") + kLensNames[i] + ",") != std::string::npos;
    if (const std::string tele = caps.String("teleTitle"); !tele.empty()) teleTitle_ = Widen(tele);
    manualExposure_ = caps.Bool("manualExposure");
    manualWhiteBalance_ = caps.Bool("manualWB");
    manualFocus_ = caps.Bool("manualFocus");
    hasTorch_ = caps.Bool("torch");
    microphoneAvailable_ = caps.Bool("microphone");
    depthSensor_ = caps.String("depth");
    // Background effects run on this PC's GPU by default: tell the phone as soon as it connects.
    Queue(Group::Effects, Format(R"({"cmd":"effects","processing":"%s"})", processing_.c_str()));
    minIso_ = std::max(1.0, caps.Number("minISO", 50));
    maxIso_ = std::max(minIso_ + 1, caps.Number("maxISO", 3200));
    minZoom_ = std::max(1.0, caps.Number("minZoom", 1));
    maxZoom_ = std::max(minZoom_ + 0.1, caps.Number("maxZoom", 10));
    minBias_ = std::max(-3.0, caps.Number("minBias", -2));
    maxBias_ = std::min(3.0, caps.Number("maxBias", 2));

    const double shortest = caps.Number("minShutterUs", 100);
    const double longest = caps.Number("maxShutterUs", 33333);
    shutterStops_.clear();
    for (double stop : kShutterStops) {
        if (stop >= shortest * 0.99 && stop <= longest * 1.01) shutterStops_.push_back(stop);
    }
    if (shutterStops_.empty()) shutterStops_.push_back(longest);
    Invalidate();
}

void StudioUi::OnState(const JsonObject& state) {
    if (!connected_) return; // capabilities always come first
    if (const std::string lens = state.String("lens"); !lens.empty() && !RecentlyTouched(Group::Lens)) lens_ = lens;
    if (!RecentlyTouched(Group::Exposure)) {
        exposureAuto_ = state.Bool("exposureAuto", true);
        iso_ = IsoT(state.Number("iso"));
        shutter_ = ShutterT(state.Number("shutterUs"));
    }
    if (!RecentlyTouched(Group::Bias) && maxBias_ > minBias_)
        bias_ = std::clamp((state.Number("bias") - minBias_) / (maxBias_ - minBias_), 0.0, 1.0);
    if (!RecentlyTouched(Group::WhiteBalance)) {
        whiteBalanceAuto_ = state.Bool("wbAuto", true);
        kelvin_ = std::clamp((state.Number("kelvin", 5000) - kMinKelvin) / (kMaxKelvin - kMinKelvin), 0.0, 1.0);
        tint_ = std::clamp((state.Number("tint") + 100) / 200, 0.0, 1.0);
    }
    if (!RecentlyTouched(Group::Focus)) {
        focusAuto_ = state.Bool("focusAuto", true);
        focus_ = std::clamp(state.Number("lensPosition"), 0.0, 1.0);
    }
    if (!RecentlyTouched(Group::Zoom)) zoom_ = ZoomT(state.Number("zoom", 1));
    if (!RecentlyTouched(Group::Torch)) torch_ = std::clamp(state.Number("torch"), 0.0, 1.0);
    torchAvailable_ = state.Bool("torchAvailable", true);
    if (!RecentlyTouched(Group::Effects) && state.Has("blur")) {
        blur_ = state.Number("blur", 0.6);
        if (state.Has("background")) background_ = state.String("background");
        else background_ = blur_ > 0.01 ? "portrait" : "off"; // older iPhone app
        if (state.Has("backdrop")) backdropName_ = state.String("backdrop");
        if (state.Has("processing")) processing_ = state.String("processing");
        keySensitivity_ = state.Number("keySensitivity", 0.5);
        keySoftness_ = state.Number("keySoftness", 0.4);
        keySpill_ = state.Number("keySpill", 0.7);
        retouch_ = state.Number("retouch");
        filterAmount_ = state.Number("filterAmount", 1);
        framing_ = state.Bool("framing");
        mirror_ = state.Bool("mirror");
        const std::string filter = state.String("filter");
        filter_ = filter.empty() ? "none" : filter;
    }
    effectsMillis_ = state.Number("effectsMs");
    visionMillis_ = state.Number("visionMs");
    if (!RecentlyTouched(Group::Audio) && state.Has("audioEnabled")) {
        microphoneOn_ = state.Bool("audioEnabled");
        microphoneDenied_ = state.Bool("microphoneDenied");
    }
    if (!RecentlyTouched(Group::Stream)) {
        if (state.Has("resolution")) resolution_ = state.String("resolution");
        if (state.Has("fps")) fps_ = static_cast<int>(state.Number("fps"));
        if (state.Has("orientation")) orientation_ = state.String("orientation");
    }
    if (!RecentlyTouched(Group::Screen)) blackout_ = state.Bool("blackout");
    if (!RecentlyTouched(Group::Prompter)) {
        prompterVisible_ = state.Bool("prompterVisible");
        prompterPlaying_ = state.Bool("prompterPlaying", true);
        prompterSpeed_ = std::clamp((state.Number("prompterSpeed", 2) - 1) / 4, 0.0, 1.0);
    }
    thermal_ = state.String("thermal");
    depthActive_ = state.Bool("depthActive");
    roundTripMillis_ = state.Has("rttMs") ? state.Number("rttMs") : -1;
    phonePaused_ = state.Bool("paused");
    Invalidate();
}

void StudioUi::OnDisconnected() {
    connected_ = false;
    lens_.clear();
    pending_.clear();
    roundTripMillis_ = -1;
    thermal_.clear();
    Invalidate();
}

void StudioUi::SetStatus(const HostStatus& status) {
    status_ = status;
    Invalidate();
}

void StudioUi::SetAudioDevices(const std::vector<AudioDevice>& devices, const std::wstring& selectedId) {
    devices_.assign(devices.begin(), devices.begin() + std::min<size_t>(devices.size(), 9));
    selectedDevice_ = selectedId;
    Invalidate();
}

void StudioUi::SetAudioLevel(float peak) {
    // Fast attack, slow release, like a hardware meter.
    const float next = peak > level_ ? peak : level_ * 0.85f;
    if (std::abs(next - level_) < 0.002f) return;
    level_ = next;
    if (tab_ == 2) Invalidate();
}

void StudioUi::SendExposure() {
    if (exposureAuto_) {
        Queue(Group::Exposure, R"({"cmd":"exposureAuto"})");
    } else {
        Queue(Group::Exposure, Format(R"({"cmd":"exposureManual","iso":%.0f,"shutterUs":%.0f})", IsoAt(iso_), ShutterAt(shutter_)));
    }
}

void StudioUi::SendWhiteBalance() {
    const double kelvin = std::round((kMinKelvin + (kMaxKelvin - kMinKelvin) * kelvin_) / 100) * 100;
    Queue(Group::WhiteBalance, whiteBalanceAuto_ ? std::string(R"({"cmd":"wbAuto"})")
                                                 : Format(R"({"cmd":"wbManual","kelvin":%.0f,"tint":%.0f})", kelvin, tint_ * 200 - 100));
}

void StudioUi::SendFocus() {
    Queue(Group::Focus, focusAuto_ ? std::string(R"({"cmd":"focusAuto"})")
                                   : Format(R"({"cmd":"focusManual","lensPosition":%.2f})", focus_));
}

void StudioUi::SendEffects() {
    Queue(Group::Effects,
          Format(R"({"cmd":"effects","processing":"%s","background":"%s","blur":%.2f,"backdrop":"%s","keySensitivity":%.2f,)"
                 R"("keySoftness":%.2f,)"
                 R"("keySpill":%.2f,"retouch":%.2f,"framing":%s,"filter":"%s","filterAmount":%.2f,"mirror":%s})",
                 processing_.c_str(), background_.c_str(), blur_, backdropName_.c_str(), keySensitivity_, keySoftness_, keySpill_,
                 retouch_,
                 framing_ ? "true" : "false", filter_.c_str(), filterAmount_, mirror_ ? "true" : "false"));
}

void StudioUi::SendPreset() {
    std::string json = R"({"cmd":"preset")";
    if (!resolution_.empty()) json += R"(,"resolution":")" + resolution_ + "\"";
    if (fps_ > 0) json += Format(R"(,"fps":%d)", fps_);
    Queue(Group::Stream, json + "}");
}

void StudioUi::SendPrompter() {
    Queue(Group::Prompter, Format(R"({"cmd":"prompter","visible":%s,"playing":%s,"speed":%.0f})", prompterVisible_ ? "true" : "false",
                                  prompterPlaying_ ? "true" : "false", 1 + prompterSpeed_ * 4));
}

void StudioUi::Touch(Group group) {
    touched_[static_cast<int>(group)] = GetTickCount64();
}

void StudioUi::Queue(Group group, std::string json) {
    const ULONGLONG now = GetTickCount64();
    Touch(group);
    pending_[group] = std::move(json);
    if (now - lastSend_ >= kSendIntervalMs) Flush();
    else SetTimer(window_, kFlushTimer, static_cast<UINT>(kSendIntervalMs), nullptr);
}

void StudioUi::Flush() {
    KillTimer(window_, kFlushTimer);
    if (pending_.empty() || !sink_) return;
    for (const auto& [group, json] : pending_) sink_(json);
    pending_.clear();
    lastSend_ = GetTickCount64();
}

bool StudioUi::RecentlyTouched(Group group) const {
    return GetTickCount64() - touched_[static_cast<int>(group)] < kTouchHoldMs;
}
