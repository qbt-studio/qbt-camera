#pragma once
#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Json.h"

// Camera controls next to the preview: lens buttons, exposure / white balance / focus with
// Auto toggles, zoom and torch. Sliders follow the phone's live values; moving one switches
// that group to manual and sends a command. Commands are coalesced to ~30 per second.
class ControlPanel {
public:
    using CommandSink = std::function<void(const std::string& json)>;
    static constexpr int kWidth = 340; // device-independent pixels

    bool Create(HWND parent, HINSTANCE instance, CommandSink sink);
    HWND Handle() const { return panel_; }

    void OnCapabilities(const JsonObject& caps);
    void OnState(const JsonObject& state);
    void OnDisconnected();
    void SetStatus(const std::wstring& text);

private:
    enum class Group { Lens, Exposure, Bias, WhiteBalance, Focus, Zoom, Torch, Count };

    struct Slider {
        HWND caption = nullptr;
        HWND track = nullptr;
        HWND value = nullptr;
    };

    struct Row {
        enum Kind { Header, Lenses, SliderRow } kind;
        HWND label = nullptr;  // header text
        HWND toggle = nullptr; // optional Auto button in a header
        Slider slider;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls(HINSTANCE instance);
    void CreateFonts();
    void Layout();
    HWND MakeLabel(const wchar_t* text, HFONT font);
    HWND MakeButton(const wchar_t* text, int id);
    Slider MakeSlider(const wchar_t* caption, int minimum, int maximum);
    void AddHeader(const wchar_t* text, HWND toggle = nullptr);
    void AddSlider(const Slider& slider);

    static int Position(const Slider& slider);
    static void SetPosition(const Slider& slider, int position);
    static void SetRange(const Slider& slider, int minimum, int maximum);
    void RefreshValueTexts();
    void RefreshEnabled();
    void RefreshButtons();

    void OnSliderMoved(HWND track);
    void OnButton(int id);
    void SendExposure();
    void SendWhiteBalance();
    void SendFocus();
    void Queue(Group group, std::string json);
    void Flush();
    bool RecentlyTouched(Group group) const;

    void DrawButton(const DRAWITEMSTRUCT& item);
    LRESULT DrawSlider(NMCUSTOMDRAW& draw);
    bool ButtonActive(HWND button) const;
    int Scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    // Value mappings between slider positions and camera units.
    double IsoAt(int position) const;
    int IsoPosition(double iso) const;
    double ZoomAt(int position) const;
    int ZoomPosition(double zoom) const;
    int ShutterPosition(double micros) const;

    HWND panel_ = nullptr;
    CommandSink sink_;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    HFONT headerFont_ = nullptr;
    HFONT statusFont_ = nullptr;
    HBRUSH background_ = nullptr;

    std::vector<Row> rows_;
    std::vector<HWND> headers_;
    std::vector<HWND> values_;
    HWND lensButtons_[4] = {};
    HWND exposureAuto_ = nullptr;
    HWND whiteBalanceAuto_ = nullptr;
    HWND focusAuto_ = nullptr;
    Slider iso_, shutter_, bias_, kelvin_, tint_, focus_, zoom_, torch_;
    HWND status_ = nullptr;

    // What the phone told us.
    bool connected_ = false;
    std::string lens_;
    bool lensAvailable_[4] = {};
    bool manualExposure_ = false, manualWhiteBalance_ = false, manualFocus_ = false, hasTorch_ = false;
    double minIso_ = 50, maxIso_ = 3200, minZoom_ = 1, maxZoom_ = 10;
    std::vector<double> shutterStops_;
    bool exposureIsAuto_ = true, whiteBalanceIsAuto_ = true, focusIsAuto_ = true;

    ULONGLONG touched_[static_cast<int>(Group::Count)] = {};
    std::map<Group, std::string> pending_;
    ULONGLONG lastSend_ = 0;
};
