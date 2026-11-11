#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "settings.h"

namespace chameleon {

enum class UiAction {
    RestartExplorer,
    OpenSettingsFolder,
    OpenSetupGuide,
    ResetDefaults,
};

struct UiStatus {
    enum class Level { Live, Waiting, Paused, Problem };
    Level level = Level::Waiting;
    std::wstring headline;
    std::wstring detail;
};

struct UiPreview {
    std::vector<std::uint32_t> pixels;  // 0xAARRGGBB, one per taskbar pixel
    bool clear = false;                 // taskbar is see-through right now
    bool frozen = false;                // sampling paused for a taskbar popup
};

class SettingsHost {
public:
    virtual ~SettingsHost() = default;
    virtual Settings& EditableSettings() = 0;
    // Called after the UI changed EditableSettings().
    virtual void SettingsEdited() = 0;
    virtual void ChoosePreset(SamplingPreset preset) = 0;
    virtual bool IsPaused() const = 0;
    virtual void SetPaused(bool paused) = 0;
    virtual bool StartsWithWindows() const = 0;
    virtual void SetStartsWithWindows(bool enabled) = 0;
    virtual UiStatus Status() const = 0;
    virtual bool Preview(UiPreview& preview) const = 0;
    virtual void Perform(UiAction action) = 0;
};

class SettingsWindow {
public:
    static constexpr wchar_t kClassName[] = L"ChameleonBar.Settings";

    SettingsWindow(HINSTANCE instance, SettingsHost* host);
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    static bool Register(HINSTANCE instance);

    void Show();
    void Hide();
    bool Visible() const;
    HWND Handle() const { return hwnd_; }

    // Settings changed outside the window (tray menu, reset).
    void Refresh();
    // Called periodically while visible to animate the live preview.
    void Tick();

    struct Impl;  // defined in ui.cpp

private:
    Impl* impl_ = nullptr;
    HWND hwnd_ = nullptr;
};

// GDI+ lifetime for the whole process.
bool StartGraphics();
void StopGraphics();

}  // namespace chameleon
