#include "StudioUi.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "Shell.h"

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

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

constexpr D2D1_COLOR_F Color(uint32_t rgb, float alpha = 1.0f) {
    return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
}
constexpr D2D1_COLOR_F White(float alpha) { return {1, 1, 1, alpha}; }

// QBT Studio palette (same as the iPhone app).
constexpr D2D1_COLOR_F kBackground = Color(0x05060B);
constexpr D2D1_COLOR_F kCard = Color(0x0A0C12);
constexpr D2D1_COLOR_F kAccent = Color(0x3A86FF);
constexpr D2D1_COLOR_F kAccentLight = Color(0x539BFF);
constexpr D2D1_COLOR_F kGreen = Color(0x34D399);
constexpr D2D1_COLOR_F kAmber = Color(0xFBBF24);
constexpr D2D1_COLOR_F kRed = Color(0xF87171);
constexpr D2D1_COLOR_F kCyan = Color(0x67E8F9);
const D2D1_COLOR_F kText = White(0.96f);
const D2D1_COLOR_F kSecondary = White(0.62f);
const D2D1_COLOR_F kTertiary = White(0.38f);

// Segoe Fluent Icons (built into Windows 11).
constexpr wchar_t kIconCamera = 0xE722, kIconVideo = 0xE714, kIconMicrophone = 0xE720, kIconVolume = 0xE767,
                  kIconBrightness = 0xE706, kIconColor = 0xE790, kIconWifi = 0xE701, kIconUsb = 0xE88E,
                  kIconTorch = 0xE754, kIconRotate = 0xE7AD, kIconCheck = 0xE73E, kIconDocument = 0xE8A5,
                  kIconSparkle = 0xF4A5, kIconBolt = 0xE945, kIconMirror = 0xE8AB, kIconThermometer = 0xE9CA,
                  kIconMonitor = 0xE7F4, kIconFace = 0xF19D, kIconTimer = 0xE916, kIconPulse = 0xE9D9,
                  kIconPhoto = 0xE91B, kIconFocus = 0xE7B3, kIconScreenOff = 0xEA14, kIconSearch = 0xE721;

// Widget ids.
enum : int {
    kTabBase = 10,
    kLensBase = 20,
    kMirrorButton = 30,
    kTorchButton = 31,
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
    // Animation slots that aren't widgets.
    kTabIndicatorX = 5000, kTabIndicatorWidth, kLensIndicatorX, kLensIndicatorWidth, kScroll,
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

std::string Format(const char* format, ...) {
    char buffer[256];
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

} // namespace

// ---------------------------------------------------------------------------------------------
// Window and resources

bool StudioUi::Create(HWND parent, HINSTANCE instance, CommandSink commands, DeviceSink audioDevice) {
    parent_ = parent;
    sink_ = std::move(commands);
    deviceSink_ = std::move(audioDevice);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    window_ = CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, parent, nullptr, instance, this);
    if (!window_) return false;
    dpi_ = GetDpiForWindow(window_);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    const auto format = [this](ComPtr<IDWriteTextFormat>& out, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                               bool wrap = false) {
        dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                  L"ru-ru", &out);
        if (out) out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    format(titleFormat_, L"Segoe UI Variable Display", 18, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format(bodyFormat_, L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_NORMAL);
    format(strongFormat_, L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format(smallFormat_, L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    format(captionFormat_, L"Segoe UI Variable Text", 11, DWRITE_FONT_WEIGHT_BOLD);
    format(bigFormat_, L"Segoe UI Variable Display", 17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    format(wrapFormat_, L"Segoe UI Variable Text", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    return CreateResources();
}

bool StudioUi::CreateResources() {
    RECT client{};
    GetClientRect(window_, &client);
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), Dpi(),
        Dpi());
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties =
        D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1)));
    if (FAILED(factory_->CreateHwndRenderTarget(properties, hwndProperties, &target_))) return false;
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->CreateSolidColorBrush(kText, &brush_);

    const auto linear = [this](ComPtr<ID2D1LinearGradientBrush>& out, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        ComPtr<ID2D1GradientStopCollection> collection;
        target_->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), &collection);
        if (collection) target_->CreateLinearGradientBrush({}, collection.Get(), &out);
    };
    // Liquid Glass: light diagonal sheen, bright top rim, accent gradient for selected items.
    linear(glassBrush_, {{0.0f, White(0.10f)}, {0.5f, White(0.035f)}, {1.0f, White(0.07f)}});
    linear(highlightBrush_, {{0.0f, White(0.38f)}, {0.35f, White(0.06f)}, {1.0f, White(0.0f)}});
    linear(activeBrush_, {{0.0f, Faded(kAccent, 0.62f)}, {0.6f, Faded(kAccent, 0.22f)}, {1.0f, Faded(kAccent, 0.42f)}});

    const auto radial = [this](ComPtr<ID2D1RadialGradientBrush>& out, D2D1_COLOR_F color) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, color}, {1.0f, Faded(color, 0.0f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        target_->CreateGradientStopCollection(stops, 2, &collection);
        if (collection) target_->CreateRadialGradientBrush({}, collection.Get(), &out);
    };
    radial(blueGlow_, Faded(Color(0x2B7FFF), 0.22f));
    radial(purpleGlow_, Faded(Color(0x9D4EDD), 0.14f));
    return brush_ && glassBrush_ && highlightBrush_ && activeBrush_ && blueGlow_ && purpleGlow_;
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
    case WM_SIZE:
        if (target_) target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
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
        if (target_) target_->SetDpi(Dpi(), Dpi());
        Invalidate();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

StudioUi::Layout StudioUi::ComputeLayout(float width, float height) const {
    constexpr float kPad = 18, kHeader = 64, kStrip = 112;
    const float sidebarWidth = std::clamp(width * 0.29f, 340.0f, 400.0f);
    Layout layout;
    layout.header = {kPad, 0, width - kPad, kHeader};
    layout.sidebar = {width - kPad - sidebarWidth, kHeader, width - kPad, height - kPad};
    const float mainRight = layout.sidebar.left - 16;
    layout.strip = {kPad, height - kPad - kStrip, mainRight, height - kPad};
    layout.card = {kPad, kHeader, mainRight, layout.strip.top - 14};
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

// ---------------------------------------------------------------------------------------------
// Frame

void StudioUi::Frame() {
    if (!target_ && !CreateResources()) return;
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

    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    const D2D1_SIZE_F size = target_->GetSize();
    const Layout layout = ComputeLayout(size.width, size.height);
    DrawBackground(size.width, size.height);
    DrawHeader(layout.header);
    DrawPreviewFrame(layout.card);
    DrawControlStrip(layout.strip);
    DrawSidebar(layout.sidebar);
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        target_.Reset();
        Invalidate();
    }

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
    target_->Clear(kBackground);
    blueGlow_->SetCenter(D2D1::Point2F(width * 0.22f, -60));
    blueGlow_->SetRadiusX(width * 0.55f);
    blueGlow_->SetRadiusY(460);
    target_->FillRectangle(D2D1::RectF(0, 0, width, height), blueGlow_.Get());
    purpleGlow_->SetCenter(D2D1::Point2F(width * 0.92f, height + 80));
    purpleGlow_->SetRadiusX(width * 0.45f);
    purpleGlow_->SetRadiusY(420);
    target_->FillRectangle(D2D1::RectF(0, 0, width, height), purpleGlow_.Get());
}

void StudioUi::DrawHeader(Rect area) {
    const float cy = (area.top + area.bottom) / 2;
    Logo(area.left, cy - 17, 34);
    const float titleWidth = Measure(L"QBT Camera", titleFormat_.Get());
    Text(L"QBT Camera", {area.left + 46, cy - 18, area.left + 46 + titleWidth + 4, cy + 4}, titleFormat_.Get(), kText);
    const Rect badge{area.left + 54 + titleWidth, cy - 15, area.left + 54 + titleWidth + 58, cy + 1};
    Fill(badge, 8, Faded(kAccent, 0.22f));
    Stroke(badge, 8, Faded(kAccent, 0.5f));
    Text(L"STUDIO", badge, captionFormat_.Get(), Color(0x9CC8FF), DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(L"iPhone как веб-камера: USB и Wi-Fi", {area.left + 46, cy + 3, area.left + 400, cy + 20}, smallFormat_.Get(),
         kTertiary);

    // Status pills, right to left.
    float right = area.right;
    const auto pill = [&](wchar_t icon, const std::wstring& text, D2D1_COLOR_F dotColor, bool dot, bool pulse) {
        const float width = Measure(text, strongFormat_.Get()) + (dot ? 46 : 52);
        const Rect rect{right - width, cy - 17, right, cy + 17};
        Glass(rect, 17, true);
        float x = rect.left + 14;
        if (dot) {
            const float breathe = pulse ? 0.5f + 0.5f * std::sin(time_ * 3.2f) : 0;
            if (pulse) {
                wantsPulse_ = true;
                Dot(x + 4, cy, 4 + 5 * breathe, Faded(dotColor, 0.35f * (1 - breathe)), false);
            }
            Dot(x + 4, cy, 4, dotColor, true);
            x += 16;
        } else {
            Icon(icon, {x - 2, cy - 9, x + 18, cy + 9}, dotColor, 15);
            x += 24;
        }
        Text(text, {x, cy - 10, rect.right - 10, cy + 10}, strongFormat_.Get(), kText);
        right = rect.left - 10;
    };

    if (!status_.cameraInstalled) pill(kIconMonitor, L"Камера не установлена", kAmber, false, false);
    else if (status_.cameraInUse) pill(0, L"Камера в работе", kRed, true, true);
    else pill(kIconMonitor, L"Камера «QBT Camera» готова", kSecondary, false, false);

    switch (status_.phase) {
    case HostStatus::Phase::Starting:
        pill(0, L"Запуск…", kTertiary, true, false);
        break;
    case HostStatus::Phase::Searching:
        pill(0, L"Ищем iPhone по USB и Wi-Fi", kAmber, true, true);
        break;
    case HostStatus::Phase::Connected:
        pill(status_.usb ? kIconUsb : kIconWifi, std::wstring(L"В эфире · ") + (status_.usb ? L"USB" : L"Wi-Fi"), kGreen,
             false, false);
        break;
    case HostStatus::Phase::Paused:
        pill(0, L"Пауза: приложение на iPhone свёрнуто", kAmber, true, true);
        break;
    case HostStatus::Phase::Error:
        pill(0, status_.error, kRed, true, false);
        break;
    }
}

void StudioUi::DrawPreviewFrame(Rect card) {
    // Soft shadow, card, glass rim. The video itself (a composition visual) covers the inside.
    for (int i = 3; i >= 1; --i) Fill(Inflate(card, i * 4.0f), 20 + i * 4.0f, Color(0x000000, 0.10f));
    Fill(card, 20, kCard);
    Stroke(card, 20, White(0.14f));
}

void StudioUi::DrawControlStrip(Rect area) {
    // Row 1: lens switcher, mirror and torch, current quality.
    const float rowTop = area.top, rowBottom = area.top + 44;
    const bool anyLensKnown = std::any_of(std::begin(lensAvailable_), std::end(lensAvailable_), [](bool b) { return b; });
    const std::wstring lensTitles[4] = {L"0.5×", L"1×", teleTitle_, L"Фронт"};
    float itemsWidth = 0;
    float itemWidths[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (anyLensKnown && !lensAvailable_[i]) continue;
        itemWidths[i] = Measure(lensTitles[i], strongFormat_.Get()) + 30;
        itemsWidth += itemWidths[i];
    }
    const Rect pill{area.left, rowTop, area.left + itemsWidth + 8, rowBottom};
    Glass(pill, 22, true);
    float x = pill.left + 4;
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
        ActiveFill({indicatorX, rowTop + 4, indicatorX + indicatorWidth, rowBottom - 4}, 18, 1);
    }
    x = pill.left + 4;
    for (int i = 0; i < 4; ++i) {
        if (itemWidths[i] == 0) continue;
        const Rect item{x, rowTop + 4, x + itemWidths[i], rowBottom - 4};
        const bool enabled = connected_ && lensAvailable_[i];
        if (Button(kLensBase + i, item, enabled) && lens_ != kLensNames[i]) {
            lens_ = kLensNames[i];
            Queue(Group::Lens, Format(R"({"cmd":"lens","value":"%s"})", kLensNames[i]));
            Flush();
        }
        const float hover = Animate(kLensBase + i + kHoverOffset, enabled && Hit(item) && i != selected ? 1.0f : 0.0f);
        if (hover > 0.01f) Fill(item, 18, White(0.08f * hover));
        Text(lensTitles[i], item, strongFormat_.Get(),
             !enabled ? kTertiary : i == selected ? kText : Mix(White(0.65f), kText, hover), DWRITE_TEXT_ALIGNMENT_CENTER);
        x += itemWidths[i];
    }

    // Round buttons next to the lenses.
    x = pill.right + 10;
    const auto roundButton = [&](int id, wchar_t icon, bool active, bool enabled) {
        const Rect rect{x, rowTop, x + 44, rowBottom};
        x += 52;
        const bool clicked = Button(id, rect, enabled);
        Glass(rect, 22, true);
        ActiveFill(rect, 22, Animate(id + kSelectOffset, active ? 1.0f : 0.0f));
        const float hover = Animate(id + kHoverOffset, enabled && Hit(rect) ? 1.0f : 0.0f);
        if (hover > 0.01f) Fill(rect, 22, White(0.07f * hover));
        Icon(icon, rect, enabled ? kText : kTertiary, 16);
        return clicked;
    };
    if (roundButton(kMirrorButton, kIconMirror, mirror_, connected_)) {
        mirror_ = !mirror_;
        SendEffects();
    }
    const bool torchOn = torch_ > 0.01;
    if (roundButton(kTorchButton, kIconTorch, torchOn, connected_ && hasTorch_ && (torchAvailable_ || torchOn))) {
        torch_ = torchOn ? 0 : 0.5;
        Touch(Group::Torch);
        Queue(Group::Torch, Format(R"({"cmd":"torch","value":%.2f})", torch_));
    }

    // Quality badge on the right.
    std::wstring quality;
    if (status_.phase == HostStatus::Phase::Connected && status_.height > 0)
        quality = WideFormat(L"%d×%d · %.0f к/с", status_.width, status_.height, status_.fps);
    if (!quality.empty()) {
        const float width = Measure(quality, strongFormat_.Get()) + 28;
        const Rect badge{area.right - width, rowTop + 5, area.right, rowBottom - 5};
        Glass(badge, 17);
        Text(quality, badge, strongFormat_.Get(), kText, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (connected_ && hasTorch_ && !torchAvailable_ && !torchOn) {
        Text(L"Фонарик заблокирован: iPhone нагрелся", {x, rowTop, area.right - 200, rowBottom}, smallFormat_.Get(), kAmber);
    }

    // Row 2: telemetry tiles.
    const float tilesTop = rowBottom + 12;
    const float gap = 10;
    const float tileWidth = (area.right - area.left - gap * 3) / 4;
    const auto tile = [&](int index, wchar_t icon, D2D1_COLOR_F iconColor, const wchar_t* label, const std::wstring& value,
                          D2D1_COLOR_F valueColor) {
        const float left = area.left + index * (tileWidth + gap);
        const Rect rect{left, tilesTop, left + tileWidth, area.bottom};
        Glass(rect, 18);
        const Rect iconBox{left + 12, tilesTop + 12, left + 44, area.bottom - 12};
        Fill(iconBox, 10, Faded(iconColor, 0.14f));
        Icon(icon, iconBox, iconColor, 16);
        Text(label, {left + 54, tilesTop + 9, rect.right - 8, tilesTop + 26}, smallFormat_.Get(), kSecondary);
        Text(value, {left + 54, tilesTop + 25, rect.right - 8, area.bottom - 6}, bigFormat_.Get(), valueColor);
    };
    const bool live = status_.phase == HostStatus::Phase::Connected;
    tile(0, kIconTimer, kGreen, L"Задержка",
         roundTripMillis_ >= 0 && live ? WideFormat(L"%.0f мс", roundTripMillis_) : L"—",
         roundTripMillis_ < 80 ? kText : kAmber);
    tile(1, kIconPulse, kCyan, L"Поток", live ? WideFormat(L"%.1f Мбит/с", status_.megabits) : L"—", kText);
    tile(2, kIconVideo, kAccentLight, L"Кадры", live ? WideFormat(L"%.0f к/с", status_.fps) : L"—", kText);
    std::wstring heat = L"—";
    D2D1_COLOR_F heatColor = kText;
    if (connected_ && thermal_ == "nominal") heat = L"Норма";
    else if (connected_ && thermal_ == "fair") heat = L"Тёплый";
    else if (connected_ && thermal_ == "serious") heat = L"Горячий", heatColor = kAmber;
    else if (connected_ && thermal_ == "critical") heat = L"Перегрев", heatColor = kRed;
    tile(3, kIconThermometer, kAmber, L"Нагрев iPhone", heat, heatColor);
}

void StudioUi::DrawSidebar(Rect area) {
    Glass(area, 28, true);

    // Tab bar.
    const Rect bar{area.left + 12, area.top + 12, area.right - 12, area.top + 12 + 58};
    Well(bar, 20);
    const wchar_t icons[4] = {kIconCamera, kIconSparkle, kIconMicrophone, kIconBolt};
    const wchar_t* titles[4] = {L"Камера", L"Эффекты", L"Звук", L"Поток"};
    const float tabWidth = (bar.right - bar.left - 8) / 4;
    const float indicatorX = Animate(kTabIndicatorX, bar.left + 4 + tab_ * tabWidth, 16);
    ActiveFill({indicatorX, bar.top + 4, indicatorX + tabWidth, bar.bottom - 4}, 16, 1);
    for (int i = 0; i < 4; ++i) {
        const Rect item{bar.left + 4 + i * tabWidth, bar.top + 4, bar.left + 4 + (i + 1) * tabWidth, bar.bottom - 4};
        if (Button(kTabBase + i, item) && tab_ != i) {
            tab_ = i;
            scroll_ = scrollTarget_ = 0;
            animations_[kScroll] = 0;
        }
        const float hover = Animate(kTabBase + i + kHoverOffset, Hit(item) && i != tab_ ? 1.0f : 0.0f);
        if (hover > 0.01f) Fill(item, 16, White(0.06f * hover));
        const D2D1_COLOR_F color = i == tab_ ? kText : Mix(White(0.55f), kText, hover);
        Icon(icons[i], {item.left, item.top + 6, item.right, item.top + 26}, color, 16);
        Text(titles[i], {item.left, item.top + 27, item.right, item.bottom - 4}, captionFormat_.Get(), color,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Scrollable content.
    const Rect viewport{area.left + 14, bar.bottom + 12, area.right - 14, area.bottom - 14};
    const float viewportHeight = viewport.bottom - viewport.top;
    if (wheel_ != 0 && Hit(viewport)) scrollTarget_ -= wheel_ / WHEEL_DELTA * 72;
    scrollTarget_ = std::clamp(scrollTarget_, 0.0f, std::max(0.0f, contentHeight_ - viewportHeight));
    scroll_ = Animate(kScroll, scrollTarget_, 18);

    target_->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    clip_ = viewport;
    const float top = viewport.top - scroll_;
    const float width = viewport.right - viewport.left;
    float bottom = top;
    switch (tab_) {
    case 0: bottom = CameraTab(viewport.left, top, width); break;
    case 1: bottom = EffectsTab(viewport.left, top, width); break;
    case 2: bottom = AudioTab(viewport.left, top, width); break;
    default: bottom = StreamTab(viewport.left, top, width); break;
    }
    contentHeight_ = bottom - top;
    clip_ = {-1e6f, -1e6f, 1e6f, 1e6f};
    target_->PopAxisAlignedClip();

    if (contentHeight_ > viewportHeight + 1) { // slim scroll indicator
        const float thumbHeight = std::max(36.0f, viewportHeight * viewportHeight / contentHeight_);
        const float travel = viewportHeight - thumbHeight;
        const float thumbTop = viewport.top + travel * (scroll_ / std::max(1.0f, contentHeight_ - viewportHeight));
        Fill({area.right - 8, thumbTop, area.right - 5, thumbTop + thumbHeight}, 1.5f, White(0.22f));
    }
}

// ---------------------------------------------------------------------------------------------
// Tabs

namespace {
constexpr float kWellPad = 12, kHeaderHeight = 32, kSliderHeight = 50, kSwitchHeight = 40, kChipHeight = 34, kGap = 12;
}

float StudioUi::CameraTab(float x, float y, float width) {
    const float ix = x + kWellPad, iw = width - kWellPad * 2;
    bool changed = false;

    // Exposure.
    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSliderHeight * 3 + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconBrightness, L"ЭКСПОЗИЦИЯ");
        Text(L"Авто", {ix + iw - 110, row - 28, ix + iw - 52, row - 8}, smallFormat_.Get(), kSecondary,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (Switch(kExposureAuto, ix + iw, row - 18, exposureAuto_, connected_ && manualExposure_)) SendExposure();
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
        row = SliderRow(kBias, ix, row, iw, L"Яркость", WideFormat(L"%+.1f EV", ev), bias_, connected_ && exposureAuto_, changed);
        if (changed) Queue(Group::Bias, Format(R"({"cmd":"exposureBias","value":%.1f})", ev));
        y = row + 4 + kGap;
    }

    // White balance.
    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSliderHeight * 2 + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconColor, L"БАЛАНС БЕЛОГО");
        Text(L"Авто", {ix + iw - 110, row - 28, ix + iw - 52, row - 8}, smallFormat_.Get(), kSecondary,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (Switch(kWhiteBalanceAuto, ix + iw, row - 18, whiteBalanceAuto_, connected_ && manualWhiteBalance_))
            SendWhiteBalance();
        const D2D1_COLOR_F cold = Color(0x60A5FA), warm = Color(0xFBBF24), green = Color(0x4ADE80), magenta = Color(0xE879F9);
        const double kelvin = std::round((kMinKelvin + (kMaxKelvin - kMinKelvin) * kelvin_) / 100) * 100;
        row = SliderRow(kKelvin, ix, row, iw, L"Температура", WideFormat(L"%.0f K", kelvin), kelvin_,
                        connected_ && manualWhiteBalance_, changed, &cold, &warm);
        if (changed) {
            whiteBalanceAuto_ = false;
            SendWhiteBalance();
        }
        row = SliderRow(kTint, ix, row, iw, L"Оттенок", WideFormat(L"%+.0f", tint_ * 200 - 100), tint_,
                        connected_ && manualWhiteBalance_, changed, &green, &magenta);
        if (changed) {
            whiteBalanceAuto_ = false;
            SendWhiteBalance();
        }
        y = row + 4 + kGap;
    }

    // Focus.
    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSliderHeight + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconFocus, L"ФОКУС");
        Text(L"Авто", {ix + iw - 110, row - 28, ix + iw - 52, row - 8}, smallFormat_.Get(), kSecondary,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (Switch(kFocusAuto, ix + iw, row - 18, focusAuto_, connected_ && manualFocus_)) SendFocus();
        row = SliderRow(kFocus, ix, row, iw, L"Дистанция", WideFormat(L"%.0f %%", focus_ * 100), focus_,
                        connected_ && manualFocus_, changed);
        if (changed) {
            focusAuto_ = false;
            SendFocus();
        }
        y = row + 4 + kGap;
    }

    // Zoom and torch.
    {
        const bool torchBlocked = connected_ && hasTorch_ && !torchAvailable_ && torch_ < 0.01;
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSliderHeight * 2 + 4 + (torchBlocked ? 22 : 0)}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconSearch, L"ЗУМ И ФОНАРИК");
        row = SliderRow(kZoom, ix, row, iw, L"Увеличение", WideFormat(L"%.1f×", ZoomAt(zoom_)), zoom_, connected_, changed);
        if (changed) Queue(Group::Zoom, Format(R"({"cmd":"zoom","value":%.3f})", ZoomAt(zoom_)));
        row = SliderRow(kTorch, ix, row, iw, L"Фонарик", torch_ < 0.01 ? std::wstring(L"выкл") : WideFormat(L"%.0f %%", torch_ * 100),
                        torch_, connected_ && hasTorch_ && !torchBlocked, changed);
        if (changed) Queue(Group::Torch, Format(R"({"cmd":"torch","value":%.2f})", torch_));
        if (torchBlocked) {
            Text(L"iPhone нагрелся — iOS временно не даёт включить фонарик", {ix, row - 4, ix + iw, row + 16},
                 smallFormat_.Get(), kAmber);
            row += 22;
        }
        y = row + 4;
    }
    return y;
}

float StudioUi::EffectsTab(float x, float y, float width) {
    const float ix = x + kWellPad, iw = width - kWellPad * 2;
    bool changed = false;

    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSliderHeight + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconPhoto, L"ФОН");
        row = SliderRow(kBlur, ix, row, iw, L"Размытие", blur_ < 0.01 ? std::wstring(L"выкл") : WideFormat(L"%.0f %%", blur_ * 100),
                        blur_, connected_, changed);
        if (changed) SendEffects();
        y = row + 4 + kGap;
    }
    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSliderHeight + kSwitchHeight + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconFace, L"ЛИЦО");
        row = SliderRow(kRetouch, ix, row, iw, L"Ретушь кожи",
                        retouch_ < 0.01 ? std::wstring(L"выкл") : WideFormat(L"%.0f %%", retouch_ * 100), retouch_, connected_,
                        changed);
        if (changed) SendEffects();
        Text(L"Следить за лицом", {ix, row, ix + iw - 60, row + kSwitchHeight}, bodyFormat_.Get(), connected_ ? kText : kTertiary);
        if (Switch(kFraming, ix + iw, row + kSwitchHeight / 2, framing_, connected_)) SendEffects();
        y = row + kSwitchHeight + 4 + kGap;
    }
    {
        const float chipsHeight = kChipHeight * 2 + 8;
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + chipsHeight + 8 + kSliderHeight + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconColor, L"ЦВЕТОВОЙ ФИЛЬТР");
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
        row += chipsHeight + 8;
        row = SliderRow(kFilterAmount, ix, row, iw, L"Сила", WideFormat(L"%.0f %%", filterAmount_ * 100), filterAmount_,
                        connected_ && filter_ != "none", changed);
        if (changed) SendEffects();
        y = row + 4 + kGap;
    }
    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSwitchHeight + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconMirror, L"КАДР");
        Text(L"Отразить по горизонтали", {ix, row, ix + iw - 60, row + kSwitchHeight}, bodyFormat_.Get(),
             connected_ ? kText : kTertiary);
        if (Switch(kMirrorSwitch, ix + iw, row + kSwitchHeight / 2, mirror_, connected_)) SendEffects();
        y = row + kSwitchHeight + 4 + kGap;
    }

    std::wstring info;
    if (!connected_) info = L"Подключите iPhone, чтобы настроить эффекты.";
    else if (effectsMillis_ > 0.05) info = WideFormat(L"Эффекты считает iPhone (нейропроцессор и GPU): %.1f мс на кадр.", effectsMillis_);
    else info = L"Эффекты выключены: видео идёт без обработки.";
    return InfoText(x + 4, y, width - 8, info, kTertiary);
}

float StudioUi::AudioTab(float x, float y, float width) {
    const float ix = x + kWellPad, iw = width - kWellPad * 2;

    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + 36}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconMicrophone, L"МИКРОФОН IPHONE");
        const bool enabled = connected_ && microphoneAvailable_;
        if (Switch(kMicrophone, ix + iw, row - 18, microphoneOn_, enabled)) {
            Touch(Group::Audio);
            Queue(Group::Audio, Format(R"({"cmd":"audio","enabled":%s})", microphoneOn_ ? "true" : "false"));
        }
        // Level meter: segmented bars, green → amber → red.
        constexpr int kBars = 32;
        const float gap = 3;
        const float barWidth = (iw - gap * (kBars - 1)) / kBars;
        const float lit = microphoneOn_ ? level_ * kBars : 0;
        for (int i = 0; i < kBars; ++i) {
            const float position = static_cast<float>(i) / kBars;
            const D2D1_COLOR_F color = position < 0.68f ? kGreen : position < 0.88f ? kAmber : kRed;
            const float amount = std::clamp(lit - i, 0.0f, 1.0f);
            const Rect bar{ix + i * (barWidth + gap), row + 4, ix + i * (barWidth + gap) + barWidth, row + 22};
            Fill(bar, 2, White(0.08f));
            if (amount > 0) Fill(bar, 2, Faded(color, 0.25f + 0.75f * amount));
        }
        y = row + 36 + kGap;
    }

    {
        const float rows = static_cast<float>(devices_.size() + 1);
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + rows * 40 + 6}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconVolume, L"ГДЕ ИГРАТЬ ЗВУК НА ПК");
        for (size_t i = 0; i <= devices_.size(); ++i) {
            const std::wstring id = i == 0 ? std::wstring() : devices_[i - 1].first;
            const std::wstring name = i == 0 ? std::wstring(L"Не выводить") : devices_[i - 1].second;
            const bool selected = id == selectedDevice_;
            const Rect item{ix - 6, row + 2, ix + iw + 6, row + 38};
            if (Button(kDeviceBase + static_cast<int>(i), item) && !selected) {
                selectedDevice_ = id;
                if (deviceSink_) deviceSink_(id);
            }
            const float hover = Animate(kDeviceBase + static_cast<int>(i) + kHoverOffset, Hit(item) ? 1.0f : 0.0f);
            const float pick = Animate(kDeviceBase + static_cast<int>(i) + kSelectOffset, selected ? 1.0f : 0.0f);
            if (pick > 0.01f) Fill(item, 12, Faded(kAccent, 0.16f * pick));
            if (hover > 0.01f) Fill(item, 12, White(0.05f * hover));
            const float cy = (item.top + item.bottom) / 2;
            brush_->SetColor(Mix(White(0.3f), kAccent, pick));
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ix + 9, cy), 8, 8), brush_.Get(), 1.6f);
            if (pick > 0.01f) Dot(ix + 9, cy, 4.5f * pick, kAccent, false);
            Text(name, {ix + 28, item.top, item.right - 8, item.bottom}, bodyFormat_.Get(), selected ? kText : kSecondary);
            row += 40;
        }
        y = row + 6 + kGap;
    }

    const auto isCable = [](const std::wstring& name) { return name.find(L"CABLE Input") != std::wstring::npos; };
    const auto selected = std::find_if(devices_.begin(), devices_.end(),
                                       [&](const AudioDevice& d) { return d.first == selectedDevice_; });
    std::wstring info;
    D2D1_COLOR_F color = kTertiary;
    if (microphoneDenied_) {
        info = L"iPhone не дал доступ к микрофону: Настройки → QBT Camera → Микрофон.";
        color = kAmber;
    } else if (selected != devices_.end() && isCable(selected->second)) {
        info = L"В Zoom, Discord или Teams выберите микрофон «CABLE Output».";
    } else if (selected == devices_.end()) {
        info = L"Звук iPhone принимается, но никуда не выводится. Свой микрофон «QBT Camera» для звонков "
               L"появится в одной из следующих версий.";
    } else {
        info = L"Звук играет в «" + selected->second + L"» — это прослушка, не микрофон для звонков.";
    }
    return InfoText(x + 4, y, width - 8, info, color);
}

float StudioUi::StreamTab(float x, float y, float width) {
    const float ix = x + kWellPad, iw = width - kWellPad * 2;
    bool changed = false;

    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kChipHeight * 2 + 8 + 10}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconVideo, L"КАЧЕСТВО");
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
        y = row + kChipHeight + 10 + kGap;
    }
    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kChipHeight * 2 + 8 + 10}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconRotate, L"ОРИЕНТАЦИЯ");
        const float half = (iw - 8) / 2;
        for (int i = 0; i < 4; ++i) {
            const float cx = ix + (i % 2) * (half + 8);
            const float cy = row + (i / 2) * (kChipHeight + 8);
            if (Chip(kOrientationBase + i, {cx, cy, cx + half, cy + kChipHeight}, kOrientationTitles[i],
                     orientation_ == kOrientationNames[i], connected_)) {
                orientation_ = kOrientationNames[i];
                Touch(Group::Stream);
                Queue(Group::Stream, Format(R"({"cmd":"orientation","value":"%s"})", kOrientationNames[i]));
            }
        }
        y = row + kChipHeight * 2 + 8 + 10 + kGap;
    }
    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + 50 + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconScreenOff, L"ЭКРАН IPHONE");
        Text(L"Чёрный экран", {ix, row + 2, ix + iw - 60, row + 24}, bodyFormat_.Get(), connected_ ? kText : kTertiary);
        Text(L"Экономит батарею и снижает нагрев", {ix, row + 23, ix + iw - 60, row + 42}, smallFormat_.Get(), kTertiary);
        if (Switch(kBlackout, ix + iw, row + 22, blackout_, connected_)) {
            Touch(Group::Screen);
            Queue(Group::Screen, Format(R"({"cmd":"screen","blackout":%s})", blackout_ ? "true" : "false"));
        }
        y = row + 50 + 4 + kGap;
    }
    {
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSwitchHeight * 2 + kSliderHeight + 4}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconDocument, L"СУФЛЁР У ОБЪЕКТИВА");
        Text(L"Показать на iPhone", {ix, row, ix + iw - 60, row + kSwitchHeight}, bodyFormat_.Get(), connected_ ? kText : kTertiary);
        if (Switch(kPrompterVisible, ix + iw, row + kSwitchHeight / 2, prompterVisible_, connected_)) SendPrompter();
        row += kSwitchHeight;
        Text(L"Прокрутка", {ix, row, ix + iw - 60, row + kSwitchHeight}, bodyFormat_.Get(), connected_ ? kText : kTertiary);
        if (Switch(kPrompterPlaying, ix + iw, row + kSwitchHeight / 2, prompterPlaying_, connected_)) SendPrompter();
        row += kSwitchHeight;
        row = SliderRow(kPrompterSpeed, ix, row, iw, L"Скорость", WideFormat(L"%.0f из 5", 1 + prompterSpeed_ * 4),
                        prompterSpeed_, connected_, changed);
        if (changed) {
            prompterSpeed_ = std::round(prompterSpeed_ * 4) / 4;
            SendPrompter();
        }
        y = row + 4 + kGap;
    }
    {
        std::wstring camera = !status_.cameraInstalled ? L"Камера «QBT Camera» не установлена: запустите install_camera.ps1."
                              : status_.cameraInUse    ? L"Камера «QBT Camera» сейчас используется приложением."
                                                       : L"Камера «QBT Camera» видна во всех приложениях: Zoom, Teams, OBS, браузеры.";
        ComPtr<IDWriteTextLayout> layout;
        dwrite_->CreateTextLayout(camera.c_str(), static_cast<UINT32>(camera.size()), wrapFormat_.Get(), iw, 1000, &layout);
        DWRITE_TEXT_METRICS metrics{};
        if (layout) layout->GetMetrics(&metrics);
        Well({x, y, x + width, y + kWellPad + kHeaderHeight + kSwitchHeight + metrics.height + 14}, 18);
        float row = SectionHeader(ix, y + kWellPad, iw, kIconMonitor, L"КОМПЬЮТЕР");
        bool autostart = shell::IsAutostartEnabled();
        Text(L"Запускать вместе с Windows", {ix, row, ix + iw - 60, row + kSwitchHeight}, bodyFormat_.Get(), kText);
        if (Switch(kAutostart, ix + iw, row + kSwitchHeight / 2, autostart)) shell::SetAutostart(autostart);
        row += kSwitchHeight;
        row = InfoText(ix, row + 2, iw, camera, kTertiary);
        y = row + 10;
    }
    return y;
}

// ---------------------------------------------------------------------------------------------
// Drawing primitives

void StudioUi::Fill(const Rect& rect, float radius, D2D1_COLOR_F color) {
    brush_->SetColor(color);
    if (radius <= 0) target_->FillRectangle(rect, brush_.Get());
    else target_->FillRoundedRectangle({rect, radius, radius}, brush_.Get());
}

void StudioUi::Stroke(const Rect& rect, float radius, D2D1_COLOR_F color, float width) {
    brush_->SetColor(color);
    const Rect inset = Inflate(rect, -width / 2);
    target_->DrawRoundedRectangle({inset, radius, radius}, brush_.Get(), width);
}

void StudioUi::Glass(const Rect& rect, float radius, bool strong) {
    glassBrush_->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
    glassBrush_->SetEndPoint(D2D1::Point2F(rect.right, rect.bottom));
    glassBrush_->SetOpacity(strong ? 1.0f : 0.8f);
    Fill(rect, radius, Color(0x0B0E18, strong ? 0.72f : 0.55f)); // tint, so glass stays readable over the glows
    target_->FillRoundedRectangle({rect, radius, radius}, glassBrush_.Get());
    Stroke(rect, radius, White(strong ? 0.16f : 0.12f));
    // Bright rim along the top edge, fading downwards.
    highlightBrush_->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
    highlightBrush_->SetEndPoint(D2D1::Point2F(rect.left, rect.top + std::min(48.0f, rect.bottom - rect.top)));
    const Rect inset = Inflate(rect, -0.75f);
    target_->DrawRoundedRectangle({inset, radius, radius}, highlightBrush_.Get(), 1.5f);
}

void StudioUi::Well(const Rect& rect, float radius) {
    Fill(rect, radius, Color(0x000000, 0.28f));
    Stroke(rect, radius, White(0.07f));
}

void StudioUi::ActiveFill(const Rect& rect, float radius, float amount) {
    if (amount <= 0.01f) return;
    Fill(Inflate(rect, 3), radius + 3, Faded(kAccent, 0.16f * amount)); // glow
    activeBrush_->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
    activeBrush_->SetEndPoint(D2D1::Point2F(rect.right, rect.bottom));
    activeBrush_->SetOpacity(amount);
    target_->FillRoundedRectangle({rect, radius, radius}, activeBrush_.Get());
    Stroke(rect, radius, White(0.42f * amount));
    highlightBrush_->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
    highlightBrush_->SetEndPoint(D2D1::Point2F(rect.left, rect.top + (rect.bottom - rect.top) * 0.8f));
    highlightBrush_->SetOpacity(amount);
    target_->DrawRoundedRectangle({Inflate(rect, -1), radius, radius}, highlightBrush_.Get(), 1.2f);
    highlightBrush_->SetOpacity(1);
}

void StudioUi::Text(const std::wstring& text, const Rect& rect, IDWriteTextFormat* format, D2D1_COLOR_F color,
                    DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT vertical) {
    if (text.empty() || !format) return;
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(vertical);
    brush_->SetColor(color);
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void StudioUi::Icon(wchar_t glyph, const Rect& rect, D2D1_COLOR_F color, float size) {
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

void StudioUi::Logo(float x, float y, float size) {
    // QBT logo from its 100x100 viewBox: rounded-square outline with a corner badge, softly glowing.
    const auto at = [size](float units) { return units * size / 100.0f; };
    const D2D1_ROUNDED_RECT outline{D2D1::RectF(x + at(11), y + at(11), x + at(83), y + at(83)), at(24), at(24)};
    brush_->SetColor(Color(0x2B7FFF, 0.18f));
    target_->DrawRoundedRectangle(outline, brush_.Get(), at(22));
    brush_->SetColor(Color(0x2B7FFF));
    target_->DrawRoundedRectangle(outline, brush_.Get(), at(10.5f));
    brush_->SetColor(Color(0x539BFF));
    target_->FillRoundedRectangle({D2D1::RectF(x + at(66), y + at(66), x + at(94), y + at(94)), at(9), at(9)}, brush_.Get());
}

void StudioUi::Dot(float cx, float cy, float radius, D2D1_COLOR_F color, bool glow) {
    if (glow) {
        brush_->SetColor(Faded(color, 0.25f));
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius * 2, radius * 2), brush_.Get());
    }
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
    Fill(rect, radius, White(0.045f + 0.05f * hover));
    Stroke(rect, radius, White(0.07f));
    ActiveFill(rect, radius, pick);
    Text(text, rect, selected ? strongFormat_.Get() : bodyFormat_.Get(),
         !enabled ? kTertiary : Mix(kSecondary, kText, std::max(pick, hover)), DWRITE_TEXT_ALIGNMENT_CENTER);
    return clicked;
}

bool StudioUi::Switch(int id, float right, float centerY, bool& value, bool enabled) {
    const Rect rect{right - 44, centerY - 12, right, centerY + 12};
    const bool clicked = Button(id, Inflate(rect, 4), enabled);
    if (clicked) value = !value;
    const float on = Animate(id + kSwitchOffset, value ? 1.0f : 0.0f, 18);
    const float alpha = enabled ? 1.0f : 0.4f;
    Fill(rect, 12, Faded(Mix(White(0.14f), kAccent, on), alpha));
    Stroke(rect, 12, White(0.12f * alpha + 0.2f * on * alpha));
    const float knobX = rect.left + 12 + on * (rect.right - rect.left - 24);
    brush_->SetColor(Color(0x000000, 0.25f * alpha));
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY + 1), 9.5f, 9.5f), brush_.Get());
    Dot(knobX, centerY, 9, White(alpha), false);
    return clicked;
}

bool StudioUi::Slider(int id, const Rect& rect, double& value01, bool enabled, const D2D1_COLOR_F* gradientFrom,
                      const D2D1_COLOR_F* gradientTo) {
    const Rect hitRect{rect.left - 8, rect.top - 6, rect.right + 8, rect.bottom + 6};
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
    const float alpha = enabled ? 1.0f : 0.35f;
    const float cy = (rect.top + rect.bottom) / 2;
    const Rect track{rect.left, cy - 2.5f, rect.right, cy + 2.5f};
    const float knobX = rect.left + static_cast<float>(value01) * (rect.right - rect.left);
    if (gradientFrom && gradientTo) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, Faded(*gradientFrom, alpha)}, {0.5f, White(0.9f * alpha)},
                                            {1.0f, Faded(*gradientTo, alpha)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        target_->CreateGradientStopCollection(stops, 3, &collection);
        if (collection)
            target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, cy), D2D1::Point2F(rect.right, cy)),
                collection.Get(), &gradient);
        if (gradient) target_->FillRoundedRectangle({track, 2.5f, 2.5f}, gradient.Get());
    } else {
        Fill(track, 2.5f, White(0.12f));
        if (knobX > track.left + 1) {
            Fill({track.left, track.top, knobX, track.bottom}, 2.5f, Faded(kAccent, alpha));
        }
    }
    const float grow = Animate(id + kHoverOffset, enabled && (active_ == id || Hit(hitRect)) ? 1.0f : 0.0f, 20);
    if (active_ == id) Dot(knobX, cy, 15, Faded(kAccent, 0.22f), false);
    brush_->SetColor(Color(0x000000, 0.35f * alpha));
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy + 1.5f), 9 + 1.5f * grow, 9 + 1.5f * grow), brush_.Get());
    Dot(knobX, cy, 8 + 1.5f * grow, White(alpha), false);
    if (!gradientFrom) {
        brush_->SetColor(Faded(kAccent, alpha));
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), 8 + 1.5f * grow, 8 + 1.5f * grow), brush_.Get(), 2);
    }
    return changed;
}

float StudioUi::SliderRow(int id, float x, float y, float width, const wchar_t* label, const std::wstring& value, double& value01,
                          bool enabled, bool& changed, const D2D1_COLOR_F* from, const D2D1_COLOR_F* to) {
    Text(label, {x, y, x + width - 90, y + 20}, bodyFormat_.Get(), enabled ? kSecondary : kTertiary);
    Text(value, {x + width - 120, y, x + width, y + 20}, strongFormat_.Get(), enabled ? kText : kTertiary,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    changed = Slider(id, {x + 8, y + 22, x + width - 8, y + 42}, value01, enabled, from, to);
    return y + kSliderHeight;
}

float StudioUi::SectionHeader(float x, float y, float width, wchar_t icon, const wchar_t* title) {
    Icon(icon, {x - 2, y + 4, x + 18, y + 22}, kAccentLight, 14);
    Text(title, {x + 24, y + 4, x + width, y + 22}, captionFormat_.Get(), White(0.58f));
    return y + kHeaderHeight;
}

float StudioUi::InfoText(float x, float y, float width, const std::wstring& text, D2D1_COLOR_F color) {
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
        blur_ = state.Number("blur");
        retouch_ = state.Number("retouch");
        filterAmount_ = state.Number("filterAmount", 1);
        framing_ = state.Bool("framing");
        mirror_ = state.Bool("mirror");
        const std::string filter = state.String("filter");
        filter_ = filter.empty() ? "none" : filter;
    }
    effectsMillis_ = state.Number("effectsMs");
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
    Touch(Group::Exposure);
    if (exposureAuto_) {
        Queue(Group::Exposure, R"({"cmd":"exposureAuto"})");
    } else {
        Queue(Group::Exposure, Format(R"({"cmd":"exposureManual","iso":%.0f,"shutterUs":%.0f})", IsoAt(iso_), ShutterAt(shutter_)));
    }
}

void StudioUi::SendWhiteBalance() {
    Touch(Group::WhiteBalance);
    const double kelvin = std::round((kMinKelvin + (kMaxKelvin - kMinKelvin) * kelvin_) / 100) * 100;
    Queue(Group::WhiteBalance, whiteBalanceAuto_ ? std::string(R"({"cmd":"wbAuto"})")
                                                 : Format(R"({"cmd":"wbManual","kelvin":%.0f,"tint":%.0f})", kelvin, tint_ * 200 - 100));
}

void StudioUi::SendFocus() {
    Touch(Group::Focus);
    Queue(Group::Focus, focusAuto_ ? std::string(R"({"cmd":"focusAuto"})")
                                   : Format(R"({"cmd":"focusManual","lensPosition":%.2f})", focus_));
}

void StudioUi::SendEffects() {
    Touch(Group::Effects);
    Queue(Group::Effects,
          Format(R"({"cmd":"effects","blur":%.2f,"retouch":%.2f,"framing":%s,"filter":"%s","filterAmount":%.2f,"mirror":%s})", blur_,
                 retouch_, framing_ ? "true" : "false", filter_.c_str(), filterAmount_, mirror_ ? "true" : "false"));
}

void StudioUi::SendPreset() {
    Touch(Group::Stream);
    std::string json = R"({"cmd":"preset")";
    if (!resolution_.empty()) json += R"(,"resolution":")" + resolution_ + "\"";
    if (fps_ > 0) json += Format(R"(,"fps":%d)", fps_);
    Queue(Group::Stream, json + "}");
}

void StudioUi::SendPrompter() {
    Touch(Group::Prompter);
    Queue(Group::Prompter, Format(R"({"cmd":"prompter","visible":%s,"playing":%s,"speed":%.0f})", prompterVisible_ ? "true" : "false",
                                  prompterPlaying_ ? "true" : "false", 1 + prompterSpeed_ * 4));
}

void StudioUi::Touch(Group group) {
    touched_[static_cast<int>(group)] = GetTickCount64();
}

void StudioUi::Queue(Group group, std::string json) {
    const ULONGLONG now = GetTickCount64();
    touched_[static_cast<int>(group)] = now;
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
