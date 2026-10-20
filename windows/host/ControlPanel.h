#pragma once
#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Json.h"

// Controls next to the preview, on three tabs. "Камера": lens buttons, exposure / white balance /
// focus with Auto toggles, zoom and torch. "Эффекты": background blur, retouch, face tracking and
// color filters (rendered on the phone). "Звук": iPhone microphone on/off, level meter and the
// Windows device to play it on. Sliders follow the phone's live values; moving one switches that
// group to manual and sends a command. Commands are coalesced to ~30 per second.
class ControlPanel {
public:
    using CommandSink = std::function<void(const std::string& json)>;
    using DeviceSink = std::function<void(const std::wstring& deviceId)>; // empty id = don't play
    using AudioDevice = std::pair<std::wstring, std::wstring>;             // id, friendly name
    static constexpr int kWidth = 340; // device-independent pixels

    bool Create(HWND parent, HINSTANCE instance, CommandSink commands, DeviceSink audioDevice);
    HWND Handle() const { return panel_; }

    void OnCapabilities(const JsonObject& caps);
    void OnState(const JsonObject& state);
    void OnDisconnected();
    void SetStatus(const std::wstring& text);
    void SetAudioDevices(const std::vector<AudioDevice>& devices, const std::wstring& selectedId);
    void SetAudioLevel(float peak);

private:
    enum class Group { Lens, Exposure, Bias, WhiteBalance, Focus, Zoom, Torch, Effects, Audio, Count };
    static constexpr int kMaxAudioDevices = 9; // plus "Не выводить"

    struct Slider {
        HWND caption = nullptr;
        HWND track = nullptr;
        HWND value = nullptr;
    };

    struct Row {
        enum Kind { Tabs, Header, Lenses, SliderRow, Filters, Info, Meter, Devices } kind;
        HWND label = nullptr;  // header or info text
        HWND toggle = nullptr; // optional toggle button in a header
        Slider slider;
        int tab = -1;          // -1: shown on every tab
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls(HINSTANCE instance);
    void CreateFonts();
    void Layout();
    HWND MakeLabel(const wchar_t* text, HFONT font);
    HWND MakeButton(const wchar_t* text, int id);
    Slider MakeSlider(const wchar_t* caption, int minimum, int maximum);
    void AddRow(Row row);
    void AddHeader(const wchar_t* text, HWND toggle = nullptr);
    void AddSlider(const Slider& slider);
    void ShowRow(const Row& row, bool show);

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
    void SendEffects();
    void RefreshAudioInfo();
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
    DeviceSink deviceSink_;
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
    HWND tabButtons_[3] = {};
    HWND microphoneToggle_ = nullptr;
    HWND meter_ = nullptr;
    HWND deviceButtons_[kMaxAudioDevices + 1] = {};
    std::vector<AudioDevice> devices_; // index 0 of deviceButtons_ is "Не выводить"
    std::wstring selectedDevice_;
    HWND audioInfo_ = nullptr;
    float level_ = 0;
    Slider blur_, retouch_, filterAmount_;
    HWND framingToggle_ = nullptr;
    HWND filterButtons_[8] = {};
    HWND effectsInfo_ = nullptr;
    HWND status_ = nullptr;
    int currentTab_ = 0;
    int buildingTab_ = -1; // tab assigned to rows while creating controls

    // What the phone told us.
    bool connected_ = false;
    std::string lens_;
    bool lensAvailable_[4] = {};
    bool manualExposure_ = false, manualWhiteBalance_ = false, manualFocus_ = false, hasTorch_ = false;
    double minIso_ = 50, maxIso_ = 3200, minZoom_ = 1, maxZoom_ = 10;
    std::vector<double> shutterStops_;
    bool exposureIsAuto_ = true, whiteBalanceIsAuto_ = true, focusIsAuto_ = true;
    bool framingOn_ = false;
    bool microphoneAvailable_ = false, microphoneOn_ = false, microphoneDenied_ = false;
    std::string filter_ = "none";
    double effectsMillis_ = 0;

    ULONGLONG touched_[static_cast<int>(Group::Count)] = {};
    std::map<Group, std::string> pending_;
    ULONGLONG lastSend_ = 0;
};
