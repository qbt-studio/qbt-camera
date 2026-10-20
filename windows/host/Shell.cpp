#include "Shell.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace shell {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"QBT Camera";
constexpr COLORREF kLogoStroke = RGB(43, 127, 255);
constexpr COLORREF kLogoBadge = RGB(83, 155, 255);

std::wstring AutostartCommand() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return L"\"" + std::wstring(path) + L"\" --minimized";
}

} // namespace

HICON MakeLogoIcon(int size) {
    // Draw 4x larger with GDI, then average 4x4 blocks: anti-aliased edges and a real alpha channel.
    constexpr int kSuper = 4;
    const int big = size * kSuper;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = big;
    info.bmiHeader.biHeight = -big;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP canvas = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!canvas) {
        DeleteDC(dc);
        return LoadIconW(nullptr, IDI_APPLICATION);
    }
    HGDIOBJ previousBitmap = SelectObject(dc, canvas);
    ZeroMemory(bits, size_t(big) * big * 4);

    // Geometry from the logo's 100x100 SVG viewBox.
    const auto at = [big](double units) { return static_cast<int>(units * big / 100.0 + 0.5); };
    HPEN stroke = CreatePen(PS_SOLID, at(10.5), kLogoStroke);
    HGDIOBJ previousPen = SelectObject(dc, stroke);
    HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, at(11), at(11), at(83), at(83), at(48), at(48));
    HPEN badgePen = CreatePen(PS_SOLID, 1, kLogoBadge);
    HBRUSH badgeBrush = CreateSolidBrush(kLogoBadge);
    SelectObject(dc, badgePen);
    SelectObject(dc, badgeBrush);
    RoundRect(dc, at(66), at(66), at(94), at(94), at(18), at(18));
    GdiFlush();

    std::vector<uint32_t> pixels(size_t(size) * size);
    const auto* source = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int covered = 0, red = 0, green = 0, blue = 0;
            for (int sy = 0; sy < kSuper; ++sy) {
                for (int sx = 0; sx < kSuper; ++sx) {
                    const uint8_t* p = source + (size_t(y * kSuper + sy) * big + (x * kSuper + sx)) * 4;
                    if (p[0] | p[1] | p[2]) {
                        ++covered;
                        blue += p[0];
                        green += p[1];
                        red += p[2];
                    }
                }
            }
            const int alpha = covered * 255 / (kSuper * kSuper);
            // Premultiplied BGRA, as CreateIconIndirect expects for 32-bit color bitmaps.
            const auto channel = [&](int sum) { return covered ? sum / covered * alpha / 255 : 0; };
            pixels[size_t(y) * size + x] = (uint32_t(alpha) << 24) | (uint32_t(channel(red)) << 16) |
                                           (uint32_t(channel(green)) << 8) | uint32_t(channel(blue));
        }
    }
    SelectObject(dc, previousPen);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousBitmap);
    DeleteObject(stroke);
    DeleteObject(badgePen);
    DeleteObject(badgeBrush);
    DeleteObject(canvas);
    DeleteDC(dc);

    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    void* colorBits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &colorBits, nullptr, 0);
    if (!color) return LoadIconW(nullptr, IDI_APPLICATION);
    memcpy(colorBits, pixels.data(), pixels.size() * 4);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO icon{TRUE, 0, 0, mask, color};
    HICON result = CreateIconIndirect(&icon);
    DeleteObject(color);
    DeleteObject(mask);
    return result ? result : LoadIconW(nullptr, IDI_APPLICATION);
}

bool IsAutostartEnabled() {
    wchar_t value[MAX_PATH * 2] = {};
    DWORD bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS;
}

void SetAutostart(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        const std::wstring command = AutostartCommand();
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

bool TrayIcon::Add(HWND window, UINT callbackMessage, HICON icon, const wchar_t* tip) {
    data_ = {};
    data_.cbSize = sizeof(data_);
    data_.hWnd = window;
    data_.uID = 1;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data_.uCallbackMessage = callbackMessage;
    data_.hIcon = icon;
    wcsncpy_s(data_.szTip, tip, _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    return added_;
}

void TrayIcon::Restore() {
    if (added_) Shell_NotifyIconW(NIM_ADD, &data_);
}

void TrayIcon::SetTip(const wchar_t* tip) {
    if (!added_ || wcscmp(data_.szTip, tip) == 0) return;
    wcsncpy_s(data_.szTip, tip, _TRUNCATE);
    data_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &data_);
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* text) {
    if (!added_) return;
    NOTIFYICONDATAW balloon = data_;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
    balloon.hBalloonIcon = data_.hIcon;
    wcsncpy_s(balloon.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(balloon.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

void TrayIcon::Remove() {
    if (!added_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
}

} // namespace shell
