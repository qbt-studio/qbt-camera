#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "Json.h"

/// What the stream thread reports for the header and the telemetry tiles.
struct HostStatus {
    enum class Phase { Starting, Searching, Connected, Paused, Error };
    Phase phase = Phase::Starting;
    std::wstring route; // "USB" or "Wi-Fi 192.168.1.5"
    bool usb = false;
    int width = 0, height = 0;
    double fps = 0, megabits = 0;
    bool cameraInstalled = false, cameraInUse = false;
    std::wstring error;
};

// The whole QBT Camera window except the video itself, in a visionOS-like style made for Windows 11:
// Mica shows through a soft colored backdrop, glass panels float on real shadows, the header doubles
// as the title bar, and there are light and dark themes (following Windows by default).
//
// Rendering: Direct2D into a premultiplied-alpha composition swap chain under the video visual,
// so the window's Mica material stays visible. A transparent child window takes the input.
//
// Immediate-mode UI: every repaint lays out, draws and hit-tests in one pass, so state changes
// from the phone just need a repaint. Sliders follow the phone's live values; touching one switches
// that group to manual and sends a command (coalesced to ~30 per second).
class StudioUi {
public:
    using CommandSink = std::function<void(const std::string& json)>;
    using DeviceSink = std::function<void(const std::wstring& deviceId)>; // empty id = don't play
    using ThemeSink = std::function<void(bool dark)>;
    using SurfaceSink = std::function<void(IUnknown* swapChain)>; // puts the interface into the window
    using AudioDevice = std::pair<std::wstring, std::wstring>;             // id, friendly name

    bool Create(HWND parent, HINSTANCE instance, const std::wstring& settingsFolder, CommandSink commands,
                DeviceSink audioDevice, ThemeSink theme, SurfaceSink surface);
    HWND Handle() const { return window_; }
    /// Where the video goes, in parent client pixels.
    RECT PreviewRect() const;
    /// Header height in pixels: the title bar part of the window (dragging, caption buttons).
    int HeaderHeight() const;
    bool DarkTheme() const { return dark_; }
    /// Without Mica (Windows 10) the background is painted opaque.
    void SetBackdropAvailable(bool available);
    /// WM_SETTINGCHANGE: Windows switched between light and dark.
    void OnSystemThemeChanged();

    void OnCapabilities(const JsonObject& caps);
    void OnState(const JsonObject& state);
    void OnDisconnected();
    void SetStatus(const HostStatus& status);
    void SetAudioDevices(const std::vector<AudioDevice>& devices, const std::wstring& selectedId);
    void SetAudioLevel(float peak);

private:
    enum class Group { Lens, Exposure, Bias, WhiteBalance, Focus, Zoom, Torch, Effects, Audio, Stream, Prompter, Screen, Count };
    enum class ThemeMode { Auto, Light, Dark };
    using Rect = D2D1_RECT_F;
    using Color = D2D1_COLOR_F;

    // Every color the interface uses; light and dark are blended while the theme animates.
    struct Palette {
        Color base, wash, blobA, blobB, blobC;
        Color glassFill, glassSheen, glassBorder, glassRim, shadow;
        Color platter, platterBorder, separator;
        Color text, secondary, tertiary;
        Color selectedFill, selectedText, control, hover, track, accent, accentSoft;
    };
    static Palette DarkPalette();
    static Palette LightPalette();
    static Palette Blend(const Palette& from, const Palette& to, float t);

    struct Layout {
        Rect header, card, strip, sidebar;
    };
    Layout ComputeLayout(float width, float height) const;

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool CreateDevice();
    bool ResizeSwapChain(UINT width, UINT height);
    void Invalidate() { InvalidateRect(window_, nullptr, FALSE); }
    float Dpi() const { return static_cast<float>(dpi_); }
    void ApplyTheme(bool animate);
    void SaveThemeMode();

    // One frame: layout + draw + input.
    void Frame();
    void DrawBackground(float width, float height);
    void DrawHeader(Rect area);
    void DrawPreviewFrame(Rect card);
    void DrawControlStrip(Rect area);
    void DrawSidebar(Rect area);
    float CameraTab(float x, float y, float width);
    float EffectsTab(float x, float y, float width);
    float AudioTab(float x, float y, float width);
    float StreamTab(float x, float y, float width);

    // Drawing primitives.
    void Fill(const Rect& rect, float radius, Color color);
    void Stroke(const Rect& rect, float radius, Color color, float width = 1);
    void Shadow(const Rect& rect, float radius, float blur, float offsetY, Color color);
    void Glass(const Rect& rect, float radius, bool elevated = true);
    Rect Platter(float x, float y, float width, float height, const wchar_t* title);
    void Separator(float x, float y, float width);
    void Selection(const Rect& rect, float radius, float amount);
    void Text(const std::wstring& text, const Rect& rect, IDWriteTextFormat* format, Color color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT vertical = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void Icon(wchar_t glyph, const Rect& rect, Color color, float size = 16);
    float Measure(const std::wstring& text, IDWriteTextFormat* format);
    float TextHeight(const std::wstring& text, float width);
    void Logo(float x, float y, float size);
    void Dot(float cx, float cy, float radius, Color color);

    // Widgets. Each returns true when the user changed something this frame.
    bool Hit(const Rect& rect) const;
    bool Button(int id, const Rect& rect, bool enabled = true);
    bool Chip(int id, const Rect& rect, const std::wstring& text, bool selected, bool enabled = true);
    bool Switch(int id, float right, float centerY, bool& value, bool enabled = true);
    bool Slider(int id, const Rect& rect, double& value01, bool enabled = true, const Color* gradientFrom = nullptr,
                const Color* gradientTo = nullptr);
    float SliderRow(int id, float x, float y, float width, const wchar_t* label, const std::wstring& value, double& value01,
                    bool enabled, bool& changed, const Color* from = nullptr, const Color* to = nullptr);
    float SwitchRow(int id, float x, float y, float width, const wchar_t* label, bool& value, bool enabled, bool& toggled,
                    const wchar_t* hint = nullptr);
    float InfoText(float x, float y, float width, const std::wstring& text, Color color);
    float Animate(int id, float target, float speed = 14.0f);

    // Value mappings between 0…1 slider positions and camera units.
    double IsoAt(double t) const;
    double IsoT(double iso) const;
    double ZoomAt(double t) const;
    double ZoomT(double zoom) const;
    double ShutterAt(double t) const;
    double ShutterT(double micros) const;

    void SendExposure();
    void SendWhiteBalance();
    void SendFocus();
    void SendEffects();
    void SendPreset();
    void SendPrompter();
    void Queue(Group group, std::string json);
    void Flush();
    bool RecentlyTouched(Group group) const;
    void Touch(Group group);

    HWND window_ = nullptr;
    HWND parent_ = nullptr;
    UINT dpi_ = 96;
    std::wstring settingsFolder_;
    CommandSink sink_;
    DeviceSink deviceSink_;
    ThemeSink themeSink_;

    // Our own D3D device and a transparent swap chain, shown below the video visual.
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    SurfaceSink surfaceSink_;
    UINT bufferWidth_ = 0, bufferHeight_ = 0;

    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> target_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> sheenBrush_, rimBrush_;
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> blob_;
    Microsoft::WRL::ComPtr<ID2D1Effect> shadowEffect_;
    // Shadow shapes by rounded rect, rebuilt when the layout changes.
    std::map<std::tuple<int, int, int, int, int>, Microsoft::WRL::ComPtr<ID2D1CommandList>> shadowShapes_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_, bodyFormat_, strongFormat_, smallFormat_, captionFormat_,
        bigFormat_, wrapFormat_, groupFormat_;
    std::map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> iconFormats_; // by size

    // Theme.
    ThemeMode themeMode_ = ThemeMode::Auto;
    bool dark_ = true;
    bool backdrop_ = true;
    Palette palette_;

    // Input, rebuilt from window messages; consumed by the next Frame().
    D2D1_POINT_2F mouse_{-1000, -1000};
    bool mouseDown_ = false, pressed_ = false, released_ = false;
    float wheel_ = 0;
    int hot_ = 0, nextHot_ = 0, active_ = 0;
    Rect clip_{-1e6f, -1e6f, 1e6f, 1e6f};
    std::vector<Rect> headerControls_; // clickable parts of the header; the rest drags the window
    std::unordered_map<int, float> animations_;
    bool animating_ = false;
    bool wantsPulse_ = false;
    float time_ = 0;
    LARGE_INTEGER lastFrame_{};
    float deltaSeconds_ = 0;
    float scroll_ = 0, scrollTarget_ = 0, contentHeight_ = 0;
    int tab_ = 0;

    // Host side.
    HostStatus status_;
    std::vector<AudioDevice> devices_;
    std::wstring selectedDevice_;
    float level_ = 0;

    // What the phone told us (and what the user set here).
    bool connected_ = false;
    std::string lens_;
    bool lensAvailable_[4] = {};
    std::wstring teleTitle_ = L"Теле";
    bool manualExposure_ = false, manualWhiteBalance_ = false, manualFocus_ = false, hasTorch_ = false;
    bool torchAvailable_ = true;
    double minIso_ = 50, maxIso_ = 3200, minZoom_ = 1, maxZoom_ = 10, minBias_ = -2, maxBias_ = 2;
    std::vector<double> shutterStops_;
    bool exposureAuto_ = true, whiteBalanceAuto_ = true, focusAuto_ = true;
    double iso_ = 0.3, shutter_ = 0.5, bias_ = 0.5, kelvin_ = 0.45, tint_ = 0.5, focus_ = 0.5, zoom_ = 0, torch_ = 0;
    double blur_ = 0, retouch_ = 0, filterAmount_ = 1;
    bool framing_ = false, mirror_ = false;
    std::string filter_ = "none";
    double effectsMillis_ = 0;
    bool microphoneAvailable_ = false, microphoneOn_ = false, microphoneDenied_ = false;
    std::string resolution_, orientation_;
    int fps_ = 0;
    bool blackout_ = false, prompterVisible_ = false, prompterPlaying_ = true;
    double prompterSpeed_ = 0.25; // 1…5 mapped to 0…1
    std::string thermal_;
    double roundTripMillis_ = -1;
    bool phonePaused_ = false;

    ULONGLONG touched_[static_cast<int>(Group::Count)] = {};
    std::map<Group, std::string> pending_;
    ULONGLONG lastSend_ = 0;
};
