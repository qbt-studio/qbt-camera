#include "ControlPanel.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace {

constexpr wchar_t kPanelClass[] = L"QBTControlPanel";
constexpr COLORREF kBackground = RGB(22, 24, 30);
constexpr COLORREF kText = RGB(232, 234, 240);
constexpr COLORREF kSecondary = RGB(150, 156, 170);
constexpr COLORREF kHeaderText = RGB(120, 160, 255);
constexpr COLORREF kDisabledText = RGB(84, 88, 100);
constexpr COLORREF kAccent = RGB(64, 120, 255);
constexpr COLORREF kControl = RGB(44, 47, 57);
constexpr COLORREF kControlPressed = RGB(60, 64, 76);
constexpr COLORREF kControlDisabled = RGB(33, 35, 43);
constexpr COLORREF kTrack = RGB(58, 62, 74);

constexpr int kLensIdBase = 100;
constexpr int kExposureAutoId = 110;
constexpr int kWhiteBalanceAutoId = 111;
constexpr int kFocusAutoId = 112;
constexpr UINT_PTR kFlushTimer = 1;
constexpr ULONGLONG kSendIntervalMs = 33;
// After the user moves a control, ignore the phone's live value for it for a moment,
// otherwise the slider would jump back under the mouse.
constexpr ULONGLONG kTouchHoldMs = 1500;

const char* const kLensNames[4] = {"ultrawide", "wide", "tele", "front"};
const wchar_t* const kLensTitles[4] = {L"0.5×", L"1×", L"Теле", L"Фронт"};
// Shutter speeds in microseconds: 1/8000 … 1/15 (1/50 and 1/100 avoid flicker under 50 Hz lights).
constexpr double kShutterStops[] = {125, 250, 500, 1000, 2000, 4000, 5000, 8000, 10000, 16667, 20000, 33333, 40000, 66667};

std::string Format(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

void FillRoundRect(HDC dc, const RECT& rect, COLORREF color, int radius) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

bool Contains(const std::vector<HWND>& list, HWND window) {
    return std::find(list.begin(), list.end(), window) != list.end();
}

} // namespace

bool ControlPanel::Create(HWND parent, HINSTANCE instance, CommandSink sink) {
    sink_ = std::move(sink);
    background_ = CreateSolidBrush(kBackground);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = background_;
    windowClass.lpszClassName = kPanelClass;
    RegisterClassExW(&windowClass);

    panel_ = CreateWindowExW(0, kPanelClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100, parent,
                             nullptr, instance, this);
    if (!panel_) return false;
    dpi_ = GetDpiForWindow(panel_);
    CreateFonts();
    CreateControls(instance);
    OnDisconnected();
    return true;
}

LRESULT CALLBACK ControlPanel::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        reinterpret_cast<ControlPanel*>(create->lpCreateParams)->panel_ = window;
    }
    auto* self = reinterpret_cast<ControlPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ControlPanel::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_ERASEBKGND: {
        RECT client;
        GetClientRect(panel_, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, background_);
        return 1;
    }
    case WM_CTLCOLORSTATIC: { // labels and trackbar backgrounds
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        COLORREF color = Contains(headers_, control) ? kHeaderText : Contains(values_, control) ? kText : kSecondary;
        if (!IsWindowEnabled(control)) color = kDisabledText;
        SetTextColor(dc, color);
        SetBkColor(dc, kBackground);
        return reinterpret_cast<LRESULT>(background_);
    }
    case WM_DRAWITEM:
        DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) OnButton(LOWORD(wParam));
        return 0;
    case WM_HSCROLL:
        if (lParam) OnSliderMoved(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->code == NM_CUSTOMDRAW) {
            wchar_t className[32] = {};
            GetClassNameW(header->hwndFrom, className, 32);
            if (wcscmp(className, TRACKBAR_CLASSW) == 0) return DrawSlider(*reinterpret_cast<NMCUSTOMDRAW*>(lParam));
        }
        break;
    }
    case WM_TIMER:
        if (wParam == kFlushTimer) Flush();
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        dpi_ = GetDpiForWindow(panel_);
        CreateFonts();
        for (HWND child = GetWindow(panel_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            HFONT font = Contains(headers_, child) ? headerFont_ : child == status_ ? statusFont_ : font_;
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        Layout();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(panel_, message, wParam, lParam);
}

void ControlPanel::CreateFonts() {
    for (HFONT font : {font_, headerFont_, statusFont_}) {
        if (font) DeleteObject(font);
    }
    auto make = [this](int size, int weight) {
        return CreateFontW(-Scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    font_ = make(14, FW_NORMAL);
    headerFont_ = make(12, FW_BOLD);
    statusFont_ = make(12, FW_NORMAL);
}

HWND ControlPanel::MakeLabel(const wchar_t* text, HFONT font) {
    HWND label = CreateWindowExW(0, WC_STATICW, text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10,
                                 panel_, nullptr, nullptr, nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return label;
}

HWND ControlPanel::MakeButton(const wchar_t* text, int id) {
    HWND button = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                  panel_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    return button;
}

ControlPanel::Slider ControlPanel::MakeSlider(const wchar_t* caption, int minimum, int maximum) {
    Slider slider;
    slider.caption = MakeLabel(caption, font_);
    slider.track = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                   0, 0, 10, 10, panel_, nullptr, nullptr, nullptr);
    slider.value = MakeLabel(L"", font_);
    SetWindowLongPtrW(slider.value, GWL_STYLE, GetWindowLongPtrW(slider.value, GWL_STYLE) | SS_RIGHT);
    values_.push_back(slider.value);
    SetRange(slider, minimum, maximum);
    return slider;
}

void ControlPanel::AddHeader(const wchar_t* text, HWND toggle) {
    HWND label = MakeLabel(text, headerFont_);
    headers_.push_back(label);
    rows_.push_back({Row::Header, label, toggle, {}});
}

void ControlPanel::AddSlider(const Slider& slider) {
    rows_.push_back({Row::SliderRow, nullptr, nullptr, slider});
}

void ControlPanel::CreateControls(HINSTANCE) {
    AddHeader(L"КАМЕРА");
    for (int i = 0; i < 4; ++i) lensButtons_[i] = MakeButton(kLensTitles[i], kLensIdBase + i);
    rows_.push_back({Row::Lenses, nullptr, nullptr, {}});

    exposureAuto_ = MakeButton(L"Авто", kExposureAutoId);
    AddHeader(L"ЭКСПОЗИЦИЯ", exposureAuto_);
    AddSlider(iso_ = MakeSlider(L"ISO", 0, 1000));
    AddSlider(shutter_ = MakeSlider(L"Выдержка", 0, 0));
    AddSlider(bias_ = MakeSlider(L"Яркость", -20, 20));

    whiteBalanceAuto_ = MakeButton(L"Авто", kWhiteBalanceAutoId);
    AddHeader(L"БАЛАНС БЕЛОГО", whiteBalanceAuto_);
    AddSlider(kelvin_ = MakeSlider(L"Температура", 25, 80));
    AddSlider(tint_ = MakeSlider(L"Оттенок", -100, 100));

    focusAuto_ = MakeButton(L"Авто", kFocusAutoId);
    AddHeader(L"ФОКУС", focusAuto_);
    AddSlider(focus_ = MakeSlider(L"Дистанция", 0, 100));

    AddHeader(L"ЗУМ");
    AddSlider(zoom_ = MakeSlider(L"Увеличение", 0, 1000));

    AddHeader(L"ФОНАРИК");
    AddSlider(torch_ = MakeSlider(L"Яркость", 0, 100));

    status_ = MakeLabel(L"", statusFont_);
    SetPosition(kelvin_, 50);
}

void ControlPanel::Layout() {
    RECT client;
    GetClientRect(panel_, &client);
    const int margin = Scale(16);
    const int width = std::max<int>(client.right - 2 * margin, Scale(120));
    int y = Scale(6);

    const bool anyLensKnown = std::any_of(std::begin(lensAvailable_), std::end(lensAvailable_), [](bool b) { return b; });
    for (const Row& row : rows_) {
        switch (row.kind) {
        case Row::Header:
            y += Scale(12);
            MoveWindow(row.label, margin, y + Scale(5), width - Scale(100), Scale(18), TRUE);
            if (row.toggle) MoveWindow(row.toggle, margin + width - Scale(92), y, Scale(92), Scale(28), TRUE);
            y += Scale(34);
            break;
        case Row::Lenses: {
            int visible = 0;
            for (int i = 0; i < 4; ++i) visible += (!anyLensKnown || lensAvailable_[i]) ? 1 : 0;
            const int gap = Scale(6);
            const int buttonWidth = (width - gap * (visible - 1)) / std::max(visible, 1);
            int x = margin;
            for (int i = 0; i < 4; ++i) {
                const bool show = !anyLensKnown || lensAvailable_[i];
                ShowWindow(lensButtons_[i], show ? SW_SHOW : SW_HIDE);
                if (!show) continue;
                MoveWindow(lensButtons_[i], x, y, buttonWidth, Scale(36), TRUE);
                x += buttonWidth + gap;
            }
            y += Scale(42);
            break;
        }
        case Row::SliderRow: {
            const int captionWidth = Scale(96);
            const int valueWidth = Scale(66);
            MoveWindow(row.slider.caption, margin, y + Scale(7), captionWidth, Scale(20), TRUE);
            MoveWindow(row.slider.track, margin + captionWidth - Scale(4), y, width - captionWidth - valueWidth + Scale(4),
                       Scale(34), TRUE);
            MoveWindow(row.slider.value, margin + width - valueWidth, y + Scale(7), valueWidth, Scale(20), TRUE);
            y += Scale(34);
            break;
        }
        }
    }
    const int statusTop = std::max<int>(y + Scale(18), client.bottom - Scale(76));
    MoveWindow(status_, margin, statusTop, width, Scale(64), TRUE);
}

int ControlPanel::Position(const Slider& slider) {
    return static_cast<int>(SendMessageW(slider.track, TBM_GETPOS, 0, 0));
}

void ControlPanel::SetPosition(const Slider& slider, int position) {
    SendMessageW(slider.track, TBM_SETPOS, TRUE, position); // does not raise WM_HSCROLL
}

void ControlPanel::SetRange(const Slider& slider, int minimum, int maximum) {
    SendMessageW(slider.track, TBM_SETRANGEMIN, FALSE, minimum);
    SendMessageW(slider.track, TBM_SETRANGEMAX, TRUE, maximum);
    SendMessageW(slider.track, TBM_SETPAGESIZE, 0, std::max(1, (maximum - minimum) / 10));
}

double ControlPanel::IsoAt(int position) const {
    return minIso_ * std::pow(maxIso_ / minIso_, position / 1000.0);
}

int ControlPanel::IsoPosition(double iso) const {
    if (iso <= minIso_ || maxIso_ <= minIso_) return 0;
    return std::clamp(static_cast<int>(std::lround(std::log(iso / minIso_) / std::log(maxIso_ / minIso_) * 1000)), 0, 1000);
}

double ControlPanel::ZoomAt(int position) const {
    return minZoom_ * std::pow(maxZoom_ / minZoom_, position / 1000.0);
}

int ControlPanel::ZoomPosition(double zoom) const {
    if (zoom <= minZoom_ || maxZoom_ <= minZoom_) return 0;
    return std::clamp(static_cast<int>(std::lround(std::log(zoom / minZoom_) / std::log(maxZoom_ / minZoom_) * 1000)), 0, 1000);
}

int ControlPanel::ShutterPosition(double micros) const {
    if (shutterStops_.empty() || micros <= 0) return 0;
    int best = 0;
    for (int i = 1; i < static_cast<int>(shutterStops_.size()); ++i) {
        if (std::abs(std::log(shutterStops_[i] / micros)) < std::abs(std::log(shutterStops_[best] / micros))) best = i;
    }
    return best;
}

void ControlPanel::OnCapabilities(const JsonObject& caps) {
    connected_ = true;
    lens_ = caps.String("lens");
    const std::string lenses = "," + caps.String("lenses") + ",";
    for (int i = 0; i < 4; ++i) lensAvailable_[i] = lenses.find(std::string(",") + kLensNames[i] + ",") != std::string::npos;
    manualExposure_ = caps.Bool("manualExposure");
    manualWhiteBalance_ = caps.Bool("manualWB");
    manualFocus_ = caps.Bool("manualFocus");
    hasTorch_ = caps.Bool("torch");

    minIso_ = std::max(1.0, caps.Number("minISO", 50));
    maxIso_ = std::max(minIso_ + 1, caps.Number("maxISO", 3200));
    minZoom_ = std::max(1.0, caps.Number("minZoom", 1));
    maxZoom_ = std::max(minZoom_ + 0.1, caps.Number("maxZoom", 10));

    const double shortest = caps.Number("minShutterUs", 100);
    const double longest = caps.Number("maxShutterUs", 33333);
    shutterStops_.clear();
    for (double stop : kShutterStops) {
        if (stop >= shortest * 0.99 && stop <= longest * 1.01) shutterStops_.push_back(stop);
    }
    if (shutterStops_.empty()) shutterStops_.push_back(longest);
    SetRange(shutter_, 0, static_cast<int>(shutterStops_.size()) - 1);

    const double minBias = std::max(-3.0, caps.Number("minBias", -2));
    const double maxBias = std::min(3.0, caps.Number("maxBias", 2));
    SetRange(bias_, static_cast<int>(std::lround(minBias * 10)), static_cast<int>(std::lround(maxBias * 10)));

    Layout();
    RefreshButtons();
    RefreshEnabled();
    RefreshValueTexts();
}

void ControlPanel::OnState(const JsonObject& state) {
    if (!connected_) return; // capabilities always come first
    if (const std::string lens = state.String("lens"); !lens.empty() && !RecentlyTouched(Group::Lens)) lens_ = lens;
    if (!RecentlyTouched(Group::Exposure)) {
        exposureIsAuto_ = state.Bool("exposureAuto", true);
        SetPosition(iso_, IsoPosition(state.Number("iso")));
        SetPosition(shutter_, ShutterPosition(state.Number("shutterUs")));
    }
    if (!RecentlyTouched(Group::Bias)) SetPosition(bias_, static_cast<int>(std::lround(state.Number("bias") * 10)));
    if (!RecentlyTouched(Group::WhiteBalance)) {
        whiteBalanceIsAuto_ = state.Bool("wbAuto", true);
        SetPosition(kelvin_, static_cast<int>(std::lround(state.Number("kelvin", 5000) / 100)));
        SetPosition(tint_, static_cast<int>(std::lround(state.Number("tint"))));
    }
    if (!RecentlyTouched(Group::Focus)) {
        focusIsAuto_ = state.Bool("focusAuto", true);
        SetPosition(focus_, static_cast<int>(std::lround(state.Number("lensPosition") * 100)));
    }
    if (!RecentlyTouched(Group::Zoom)) SetPosition(zoom_, ZoomPosition(state.Number("zoom", 1)));
    if (!RecentlyTouched(Group::Torch)) SetPosition(torch_, static_cast<int>(std::lround(state.Number("torch") * 100)));
    RefreshButtons();
    RefreshEnabled();
    RefreshValueTexts();
}

void ControlPanel::OnDisconnected() {
    connected_ = false;
    lens_.clear();
    pending_.clear();
    RefreshButtons();
    RefreshEnabled();
    RefreshValueTexts();
}

void ControlPanel::SetStatus(const std::wstring& text) {
    SetWindowTextW(status_, text.c_str());
}

void ControlPanel::RefreshValueTexts() {
    wchar_t text[32];
    auto show = [&](const Slider& slider) { SetWindowTextW(slider.value, text); };

    swprintf_s(text, L"%.0f", IsoAt(Position(iso_)));
    show(iso_);
    if (shutterStops_.empty()) {
        wcscpy_s(text, L"—");
    } else {
        const double micros = shutterStops_[std::clamp<size_t>(Position(shutter_), 0, shutterStops_.size() - 1)];
        swprintf_s(text, L"1/%.0f", 1'000'000.0 / micros);
    }
    show(shutter_);
    swprintf_s(text, L"%+.1f EV", Position(bias_) / 10.0);
    show(bias_);
    swprintf_s(text, L"%d K", Position(kelvin_) * 100);
    show(kelvin_);
    swprintf_s(text, L"%+d", Position(tint_));
    show(tint_);
    swprintf_s(text, L"%d %%", Position(focus_));
    show(focus_);
    swprintf_s(text, L"%.1f×", ZoomAt(Position(zoom_)));
    show(zoom_);
    if (Position(torch_) == 0) wcscpy_s(text, L"выкл");
    else swprintf_s(text, L"%d %%", Position(torch_));
    show(torch_);
}

void ControlPanel::RefreshEnabled() {
    auto enable = [](const Slider& slider, bool on) {
        EnableWindow(slider.caption, on);
        EnableWindow(slider.track, on);
        EnableWindow(slider.value, on);
    };
    for (int i = 0; i < 4; ++i) EnableWindow(lensButtons_[i], connected_ && lensAvailable_[i]);
    EnableWindow(exposureAuto_, connected_ && manualExposure_);
    enable(iso_, connected_ && manualExposure_);
    enable(shutter_, connected_ && manualExposure_);
    enable(bias_, connected_ && exposureIsAuto_); // exposure compensation only steers auto exposure
    EnableWindow(whiteBalanceAuto_, connected_ && manualWhiteBalance_);
    enable(kelvin_, connected_ && manualWhiteBalance_);
    enable(tint_, connected_ && manualWhiteBalance_);
    EnableWindow(focusAuto_, connected_ && manualFocus_);
    enable(focus_, connected_ && manualFocus_);
    enable(zoom_, connected_);
    enable(torch_, connected_ && hasTorch_);
}

void ControlPanel::RefreshButtons() {
    SetWindowTextW(exposureAuto_, exposureIsAuto_ ? L"Авто" : L"Ручной");
    SetWindowTextW(whiteBalanceAuto_, whiteBalanceIsAuto_ ? L"Авто" : L"Ручной");
    SetWindowTextW(focusAuto_, focusIsAuto_ ? L"Авто" : L"Ручной");
    for (HWND button : lensButtons_) InvalidateRect(button, nullptr, FALSE);
    for (HWND button : {exposureAuto_, whiteBalanceAuto_, focusAuto_}) InvalidateRect(button, nullptr, FALSE);
}

bool ControlPanel::ButtonActive(HWND button) const {
    for (int i = 0; i < 4; ++i) {
        if (button == lensButtons_[i]) return connected_ && lens_ == kLensNames[i];
    }
    if (button == exposureAuto_) return exposureIsAuto_;
    if (button == whiteBalanceAuto_) return whiteBalanceIsAuto_;
    if (button == focusAuto_) return focusIsAuto_;
    return false;
}

void ControlPanel::SendExposure() {
    if (exposureIsAuto_) {
        Queue(Group::Exposure, R"({"cmd":"exposureAuto"})");
    } else {
        const double micros = shutterStops_.empty() ? 10000
                                                    : shutterStops_[std::clamp<size_t>(Position(shutter_), 0, shutterStops_.size() - 1)];
        Queue(Group::Exposure, Format(R"({"cmd":"exposureManual","iso":%.0f,"shutterUs":%.0f})", IsoAt(Position(iso_)), micros));
    }
}

void ControlPanel::SendWhiteBalance() {
    Queue(Group::WhiteBalance, whiteBalanceIsAuto_
                                   ? std::string(R"({"cmd":"wbAuto"})")
                                   : Format(R"({"cmd":"wbManual","kelvin":%d,"tint":%d})", Position(kelvin_) * 100, Position(tint_)));
}

void ControlPanel::SendFocus() {
    Queue(Group::Focus, focusIsAuto_ ? std::string(R"({"cmd":"focusAuto"})")
                                     : Format(R"({"cmd":"focusManual","lensPosition":%.2f})", Position(focus_) / 100.0));
}

void ControlPanel::OnSliderMoved(HWND track) {
    if (track == iso_.track || track == shutter_.track) {
        exposureIsAuto_ = false;
        SendExposure();
    } else if (track == bias_.track) {
        Queue(Group::Bias, Format(R"({"cmd":"exposureBias","value":%.1f})", Position(bias_) / 10.0));
    } else if (track == kelvin_.track || track == tint_.track) {
        whiteBalanceIsAuto_ = false;
        SendWhiteBalance();
    } else if (track == focus_.track) {
        focusIsAuto_ = false;
        SendFocus();
    } else if (track == zoom_.track) {
        Queue(Group::Zoom, Format(R"({"cmd":"zoom","value":%.3f})", ZoomAt(Position(zoom_))));
    } else if (track == torch_.track) {
        Queue(Group::Torch, Format(R"({"cmd":"torch","value":%.2f})", Position(torch_) / 100.0));
    }
    RefreshButtons();
    RefreshEnabled();
    RefreshValueTexts();
}

void ControlPanel::OnButton(int id) {
    if (id >= kLensIdBase && id < kLensIdBase + 4) {
        const int index = id - kLensIdBase;
        if (!lensAvailable_[index]) return;
        lens_ = kLensNames[index];
        Queue(Group::Lens, Format(R"({"cmd":"lens","value":"%s"})", kLensNames[index]));
        Flush();
    } else if (id == kExposureAutoId) {
        exposureIsAuto_ = !exposureIsAuto_;
        SendExposure();
    } else if (id == kWhiteBalanceAutoId) {
        whiteBalanceIsAuto_ = !whiteBalanceIsAuto_;
        SendWhiteBalance();
    } else if (id == kFocusAutoId) {
        focusIsAuto_ = !focusIsAuto_;
        SendFocus();
    }
    RefreshButtons();
    RefreshEnabled();
}

void ControlPanel::Queue(Group group, std::string json) {
    const ULONGLONG now = GetTickCount64();
    touched_[static_cast<int>(group)] = now;
    pending_[group] = std::move(json);
    if (now - lastSend_ >= kSendIntervalMs)
        Flush();
    else
        SetTimer(panel_, kFlushTimer, static_cast<UINT>(kSendIntervalMs), nullptr);
}

void ControlPanel::Flush() {
    KillTimer(panel_, kFlushTimer);
    if (pending_.empty() || !sink_) return;
    for (const auto& [group, json] : pending_) sink_(json);
    pending_.clear();
    lastSend_ = GetTickCount64();
}

bool ControlPanel::RecentlyTouched(Group group) const {
    return GetTickCount64() - touched_[static_cast<int>(group)] < kTouchHoldMs;
}

void ControlPanel::DrawButton(const DRAWITEMSTRUCT& item) {
    HDC dc = item.hDC;
    const RECT rect = item.rcItem;
    FillRect(dc, &rect, background_);
    const bool enabled = !(item.itemState & ODS_DISABLED);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool active = enabled && ButtonActive(item.hwndItem);
    const COLORREF fill = !enabled ? kControlDisabled : active ? kAccent : pressed ? kControlPressed : kControl;
    FillRoundRect(dc, rect, fill, Scale(12));

    wchar_t text[64] = {};
    GetWindowTextW(item.hwndItem, text, 64);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, !enabled ? kDisabledText : active ? RGB(255, 255, 255) : kText);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    RECT textRect = rect;
    DrawTextW(dc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

LRESULT ControlPanel::DrawSlider(NMCUSTOMDRAW& draw) {
    if (draw.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;

    HWND track = draw.hdr.hwndFrom;
    const bool enabled = IsWindowEnabled(track) != FALSE;
    RECT thumb{};
    SendMessageW(track, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const int centerY = (thumb.top + thumb.bottom) / 2;

    switch (draw.dwItemSpec) {
    case TBCD_TICS:
        return CDRF_SKIPDEFAULT;
    case TBCD_CHANNEL: { // thin rounded line, filled with the accent color up to the thumb
        const int half = std::max(1, Scale(2));
        const RECT whole{draw.rc.left + Scale(2), centerY - half, draw.rc.right - Scale(2), centerY + half};
        FillRoundRect(draw.hdc, whole, kTrack, half * 2);
        if (enabled) {
            const RECT filled{whole.left, whole.top, (thumb.left + thumb.right) / 2, whole.bottom};
            if (filled.right > filled.left) FillRoundRect(draw.hdc, filled, kAccent, half * 2);
        }
        return CDRF_SKIPDEFAULT;
    }
    case TBCD_THUMB: { // round knob
        const int centerX = (thumb.left + thumb.right) / 2;
        const int radius = Scale(8);
        HBRUSH brush = CreateSolidBrush(enabled ? RGB(242, 244, 250) : RGB(78, 82, 94));
        HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(2)), enabled ? kAccent : RGB(78, 82, 94));
        HGDIOBJ oldBrush = SelectObject(draw.hdc, brush);
        HGDIOBJ oldPen = SelectObject(draw.hdc, pen);
        Ellipse(draw.hdc, centerX - radius, centerY - radius, centerX + radius + 1, centerY + radius + 1);
        SelectObject(draw.hdc, oldBrush);
        SelectObject(draw.hdc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}
