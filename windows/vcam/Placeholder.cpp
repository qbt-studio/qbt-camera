#include "Placeholder.h"

namespace {

// BT.709 limited range, the same colorimetry the iPhone stream uses.
BYTE LumaOf(int r, int g, int b) { return static_cast<BYTE>(16 + ((47 * r + 157 * g + 16 * b) >> 8)); }
BYTE BlueDifferenceOf(int r, int g, int b) { return static_cast<BYTE>(128 + ((-26 * r - 87 * g + 112 * b) >> 8)); }
BYTE RedDifferenceOf(int r, int g, int b) { return static_cast<BYTE>(128 + ((112 * r - 102 * g - 10 * b) >> 8)); }

void DrawCenteredText(HDC dc, const wchar_t* text, int fontHeight, int weight, COLORREF color, RECT area) {
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ previous = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous);
    DeleteObject(font);
}

} // namespace

void Placeholder::Draw(BYTE* destination, LONG pitch, UINT32 width, UINT32 height) {
    if (width != width_ || height != height_) Render(width, height);
    for (UINT32 row = 0; row < height * 3 / 2; ++row)
        memcpy(destination + size_t(row) * pitch, nv12_.data() + size_t(row) * width, width);
}

void Placeholder::Render(UINT32 width, UINT32 height) {
    width_ = width;
    height_ = height;
    nv12_.assign(size_t(width) * height * 3 / 2, 0);
    std::vector<BYTE> bgra(size_t(width) * height * 4);
    const int backgroundR = 17, backgroundG = 19, backgroundB = 26;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bitmap && bits) {
        HGDIOBJ previous = SelectObject(dc, bitmap);
        RECT all{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        HBRUSH background = CreateSolidBrush(RGB(backgroundR, backgroundG, backgroundB));
        FillRect(dc, &all, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);

        const int unit = static_cast<int>(height) / 18;
        RECT title{0, static_cast<LONG>(height) / 2 - unit * 2, static_cast<LONG>(width), static_cast<LONG>(height) / 2};
        DrawCenteredText(dc, L"QBT Camera", unit * 2, FW_SEMIBOLD, RGB(236, 238, 245), title);
        RECT hint{0, static_cast<LONG>(height) / 2 + unit / 2, static_cast<LONG>(width),
                  static_cast<LONG>(height) / 2 + unit * 2};
        DrawCenteredText(dc, L"Откройте QBT Camera на iPhone и на компьютере", unit, FW_NORMAL, RGB(140, 146, 160), hint);

        GdiFlush();
        memcpy(bgra.data(), bits, bgra.size());
        SelectObject(dc, previous);
    } else {
        for (size_t i = 0; i < bgra.size(); i += 4) {
            bgra[i] = backgroundB;
            bgra[i + 1] = backgroundG;
            bgra[i + 2] = backgroundR;
        }
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);

    BYTE* luma = nv12_.data();
    BYTE* chroma = luma + size_t(width) * height;
    for (UINT32 y = 0; y < height; ++y) {
        for (UINT32 x = 0; x < width; ++x) {
            const BYTE* pixel = &bgra[(size_t(y) * width + x) * 4];
            luma[size_t(y) * width + x] = LumaOf(pixel[2], pixel[1], pixel[0]);
        }
    }
    for (UINT32 y = 0; y < height; y += 2) {
        for (UINT32 x = 0; x < width; x += 2) {
            int r = 0, g = 0, b = 0;
            for (UINT32 dy = 0; dy < 2; ++dy) {
                for (UINT32 dx = 0; dx < 2; ++dx) {
                    const BYTE* pixel = &bgra[(size_t(y + dy) * width + x + dx) * 4];
                    b += pixel[0];
                    g += pixel[1];
                    r += pixel[2];
                }
            }
            BYTE* uv = chroma + size_t(y / 2) * width + x;
            uv[0] = BlueDifferenceOf(r / 4, g / 4, b / 4);
            uv[1] = RedDifferenceOf(r / 4, g / 4, b / 4);
        }
    }
}
