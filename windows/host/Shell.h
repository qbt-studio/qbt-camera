#pragma once
#include <windows.h>
#include <shellapi.h>

// Windows shell integration: the QBT logo icon, the tray icon and starting with Windows.
namespace shell {

/// The QBT logo (rounded-square outline with a corner badge) as an anti-aliased icon.
HICON MakeLogoIcon(int size);

bool IsAutostartEnabled();
/// Starts QBTCamera.exe minimized to the tray at sign-in (per user, HKCU Run).
void SetAutostart(bool enabled);

class TrayIcon {
public:
    bool Add(HWND window, UINT callbackMessage, HICON icon, const wchar_t* tip);
    /// Explorer restarted ("TaskbarCreated"): put the icon back.
    void Restore();
    void SetTip(const wchar_t* tip);
    void ShowBalloon(const wchar_t* title, const wchar_t* text);
    void Remove();

private:
    NOTIFYICONDATAW data_{};
    bool added_ = false;
};

} // namespace shell
