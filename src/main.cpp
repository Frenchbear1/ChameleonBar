#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "color_sampler.h"
#include "resource.h"
#include "settings.h"
#include "surface_publisher.h"
#include "ui.h"
#include "window_activity.h"
#include "xaml_bridge_client.h"

namespace {

constexpr wchar_t kApplicationName[] = L"ChameleonBar";
constexpr wchar_t kControllerClass[] = L"ChameleonBar.Controller";
constexpr wchar_t kBackdropClass[] = L"ChameleonBar.Backdrop";

constexpr UINT kTrayMessage = WM_APP + 41;
constexpr UINT kShowSettingsMessage = WM_APP + 42;
constexpr UINT kExplorerRestartedMessage = WM_APP + 43;

constexpr UINT_PTR kSamplingTimer = 1;
constexpr UINT_PTR kTestExitTimer = 2;
constexpr UINT_PTR kSettingsSaveTimer = 3;
constexpr UINT_PTR kUiTimer = 4;
constexpr UINT_PTR kBridgeRetryTimer = 5;

constexpr UINT kMenuOpenWindow = 1000;
constexpr UINT kMenuPause = 1001;
constexpr UINT kMenuPresetBase = 1100;  // + preset index
constexpr UINT kMenuRunAtStartup = 1301;
constexpr UINT kMenuRestartExplorer = 1302;
constexpr UINT kMenuExit = 1601;

constexpr ULONGLONG kRediscoveryMilliseconds = 1800;
constexpr ULONGLONG kWindowActivityScanMilliseconds = 48;
constexpr ULONGLONG kBridgeGraceMilliseconds = 6000;
constexpr int kBridgeRetryLimit = 60;  // two minutes at 2 s

HINSTANCE g_instance = nullptr;

std::wstring ApplicationDirectory() {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
        return L".";
    }
    std::wstring path(executable, length);
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

HICON LoadApplicationIcon(int width, int height) {
    HICON icon = static_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(IDI_CHAMELEONBAR), IMAGE_ICON, width, height,
        LR_DEFAULTCOLOR | LR_SHARED));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

// --- Legacy transparency (undocumented; off by default) ----------------------

enum class AccentState : int {
    Disabled = 0,
    EnableTransparentGradient = 2,
};

struct AccentPolicy {
    AccentState state;
    DWORD flags;
    DWORD gradientColor;
    DWORD animationId;
};

struct WindowCompositionAttributeData {
    int attribute;
    PVOID data;
    SIZE_T sizeOfData;
};

using SetWindowCompositionAttributeFunction = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

void SetTaskbarTransparent(HWND taskbar, bool transparent) {
    static const auto function = reinterpret_cast<SetWindowCompositionAttributeFunction>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                               "SetWindowCompositionAttribute")));
    if (!function || !IsWindow(taskbar)) {
        return;
    }
    AccentPolicy policy{};
    policy.state = transparent ? AccentState::EnableTransparentGradient : AccentState::Disabled;
    policy.flags = transparent ? 2u : 0u;
    WindowCompositionAttributeData data{19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy)};
    function(taskbar, &data);
}

bool IsTaskbarWindow(HWND window) {
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return wcscmp(className, L"Shell_TrayWnd") == 0 ||
           wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

bool IsPrimaryTaskbar(HWND window) {
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return wcscmp(className, L"Shell_TrayWnd") == 0;
}

std::vector<HWND> EnumerateTaskbars() {
    std::vector<HWND> result;
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            if (IsTaskbarWindow(window)) {
                reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

// --- Screen capture buffer ---------------------------------------------------

class DibBuffer {
public:
    DibBuffer() = default;
    ~DibBuffer() { Reset(); }
    DibBuffer(const DibBuffer&) = delete;
    DibBuffer& operator=(const DibBuffer&) = delete;

    bool Ensure(int width, int height) {
        if (width <= 0 || height <= 0) {
            return false;
        }
        if (width_ == width && height_ == height && bitmap_ && memoryDc_) {
            return true;
        }
        Reset();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        memoryDc_ = CreateCompatibleDC(screen);
        bitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!memoryDc_ || !bitmap_ || !bits_) {
            Reset();
            return false;
        }
        oldBitmap_ = SelectObject(memoryDc_, bitmap_);
        width_ = width;
        height_ = height;
        stride_ = width * 4;
        return true;
    }

    void Reset() {
        if (memoryDc_ && oldBitmap_) {
            SelectObject(memoryDc_, oldBitmap_);
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
        }
        if (memoryDc_) {
            DeleteDC(memoryDc_);
        }
        memoryDc_ = nullptr;
        bitmap_ = nullptr;
        oldBitmap_ = nullptr;
        bits_ = nullptr;
        width_ = height_ = stride_ = 0;
    }

    HDC dc() const { return memoryDc_; }
    void* bits() const { return bits_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }

private:
    HDC memoryDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
};

// --- Geometry ----------------------------------------------------------------

struct Geometry {
    RECT taskbar{};
    RECT sample{};
    chameleon::AdjacentEdge adjacentEdge = chameleon::AdjacentEdge::Bottom;
    bool horizontal = true;
};

bool ResolveGeometry(HWND taskbar, int requestedDepth, int requestedOffset, Geometry& geometry) {
    if (!IsWindow(taskbar) || !GetWindowRect(taskbar, &geometry.taskbar)) {
        return false;
    }
    HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    const int taskbarWidth = geometry.taskbar.right - geometry.taskbar.left;
    const int taskbarHeight = geometry.taskbar.bottom - geometry.taskbar.top;
    if (taskbarWidth <= 0 || taskbarHeight <= 0) {
        return false;
    }

    const RECT monitorRect = info.rcMonitor;
    geometry.horizontal = taskbarWidth >= taskbarHeight;
    const int depth = std::max(1, requestedDepth);
    const int offset = std::max(0, requestedOffset);

    if (geometry.horizontal) {
        const int distanceToTop = std::abs(geometry.taskbar.top - monitorRect.top);
        const int distanceToBottom = std::abs(monitorRect.bottom - geometry.taskbar.bottom);
        if (distanceToTop <= distanceToBottom) {
            geometry.adjacentEdge = chameleon::AdjacentEdge::Top;
            geometry.sample = {
                geometry.taskbar.left,
                std::min(monitorRect.bottom, geometry.taskbar.bottom + offset),
                geometry.taskbar.right,
                std::min(monitorRect.bottom, geometry.taskbar.bottom + offset + depth),
            };
        } else {
            geometry.adjacentEdge = chameleon::AdjacentEdge::Bottom;
            geometry.sample = {
                geometry.taskbar.left,
                std::max(monitorRect.top, geometry.taskbar.top - offset - depth),
                geometry.taskbar.right,
                std::max(monitorRect.top, geometry.taskbar.top - offset),
            };
        }
    } else {
        const int distanceToLeft = std::abs(geometry.taskbar.left - monitorRect.left);
        const int distanceToRight = std::abs(monitorRect.right - geometry.taskbar.right);
        if (distanceToLeft <= distanceToRight) {
            geometry.adjacentEdge = chameleon::AdjacentEdge::Left;
            geometry.sample = {
                std::min(monitorRect.right, geometry.taskbar.right + offset),
                geometry.taskbar.top,
                std::min(monitorRect.right, geometry.taskbar.right + offset + depth),
                geometry.taskbar.bottom,
            };
        } else {
            geometry.adjacentEdge = chameleon::AdjacentEdge::Right;
            geometry.sample = {
                std::max(monitorRect.left, geometry.taskbar.left - offset - depth),
                geometry.taskbar.top,
                std::max(monitorRect.left, geometry.taskbar.left - offset),
                geometry.taskbar.bottom,
            };
        }
    }
    return geometry.sample.right > geometry.sample.left &&
           geometry.sample.bottom > geometry.sample.top;
}

std::vector<int> ApplicationBoundariesForSample(
    const Geometry& geometry, const std::vector<RECT>& applicationWindows) {
    std::vector<int> boundaries;
    const int length = geometry.horizontal
        ? static_cast<int>(geometry.sample.right - geometry.sample.left)
        : static_cast<int>(geometry.sample.bottom - geometry.sample.top);
    for (const RECT& application : applicationWindows) {
        RECT intersection{};
        if (!IntersectRect(&intersection, &geometry.sample, &application)) {
            continue;
        }
        if (geometry.horizontal) {
            boundaries.push_back(std::clamp(static_cast<int>(application.left - geometry.sample.left), 0, length));
            boundaries.push_back(std::clamp(static_cast<int>(application.right - geometry.sample.left), 0, length));
        } else {
            boundaries.push_back(std::clamp(static_cast<int>(application.top - geometry.sample.top), 0, length));
            boundaries.push_back(std::clamp(static_cast<int>(application.bottom - geometry.sample.top), 0, length));
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

// --- One taskbar ---------------------------------------------------------------

// Samples the screen beside one taskbar and publishes the result. Also owns a
// click-through fallback window that is only shown if the bridge is missing.
class Backdrop {
public:
    Backdrop(HWND taskbar, chameleon::SurfacePublisher* publisher)
        : taskbar_(taskbar), publisher_(publisher) {
        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
            kBackdropClass, L"ChameleonBar backdrop", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, g_instance, this);
        if (hwnd_) {
            const BOOL yes = TRUE;
            DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof(yes));
            DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &yes, sizeof(yes));
        }
    }

    ~Backdrop() {
        if (publisher_) {
            publisher_->Release(taskbar_);
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
        }
    }

    Backdrop(const Backdrop&) = delete;
    Backdrop& operator=(const Backdrop&) = delete;

    HWND taskbar() const { return taskbar_; }
    bool valid() const { return IsWindow(taskbar_) && IsWindow(hwnd_); }
    bool primary() const { return IsPrimaryTaskbar(taskbar_); }
    const std::vector<std::uint32_t>& lastColors() const { return lastColors_; }
    chameleon::SurfaceMode lastMode() const { return lastMode_; }
    bool frozen() const { return frozen_; }
    void SetFrozen(bool frozen) { frozen_ = frozen; }

    // Hands the taskbar back to Windows.
    void Restore() {
        if (publisher_) {
            publisher_->SetNative(taskbar_);
        }
        HideFallback();
        temporal_.Reset();
        lastColors_.clear();
        lastMode_ = chameleon::kSurfaceNative;
        frozen_ = false;
    }

    void HideFallback() {
        if (hwnd_ && IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }

    bool Update(const chameleon::RuntimeTuning& tuning,
                double elapsedMilliseconds,
                bool showFallback,
                const std::vector<RECT>& applicationWindows,
                chameleon::SurfaceMode mode) {
        frozen_ = false;
        Geometry geometry;
        if (!ResolveGeometry(taskbar_, tuning.sampleDepth, tuning.sampleOffset, geometry)) {
            Restore();
            return false;
        }
        const int sampleWidth = geometry.sample.right - geometry.sample.left;
        const int sampleHeight = geometry.sample.bottom - geometry.sample.top;
        if (!capture_.Ensure(sampleWidth, sampleHeight)) {
            return false;
        }

        // No CAPTUREBLT: with DWM composition it adds nothing and makes the
        // mouse cursor flicker on many systems.
        HDC screen = GetDC(nullptr);
        const BOOL copied = BitBlt(capture_.dc(), 0, 0, sampleWidth, sampleHeight, screen,
                                   geometry.sample.left, geometry.sample.top, SRCCOPY);
        ReleaseDC(nullptr, screen);
        if (!copied) {
            return false;
        }
        GdiFlush();

        chameleon::SamplerConfig config;
        config.cellWidth = tuning.cellWidth;
        config.edgeThreshold = tuning.edgeThreshold;
        config.horizontalBlurPixels = tuning.horizontalBlurPixels;
        config.blurStrength = tuning.blurStrength;
        config.backgroundOnly = tuning.backgroundOnly;
        config.detailRejectionPixels = tuning.detailRejectionPixels;
        config.protectedBoundaryPixels = ApplicationBoundariesForSample(geometry, applicationWindows);
        auto target = chameleon::ExtractColorField(
            static_cast<const std::uint8_t*>(capture_.bits()), sampleWidth, sampleHeight,
            capture_.stride(), geometry.adjacentEdge, config);
        if (target.empty()) {
            return false;
        }
        chameleon::AdjustColorField(target, tuning.saturation, tuning.brightness);

        const auto& filtered = temporal_.Update(target, elapsedMilliseconds, tuning.transitionMilliseconds);
        const int taskbarWidth = geometry.taskbar.right - geometry.taskbar.left;
        const int taskbarHeight = geometry.taskbar.bottom - geometry.taskbar.top;
        const int outputLength = geometry.horizontal ? taskbarWidth : taskbarHeight;
        auto colors = chameleon::ExpandColorField(
            filtered, outputLength, tuning.cellWidth, tuning.edgeThreshold,
            tuning.horizontalBlurPixels > 0 && tuning.blurStrength > 0.0f);
        if (colors.empty()) {
            return false;
        }
        // Identical frames are not republished, so Explorer does no work while
        // the screen is static. A periodic refresh keeps the slot fresh.
        const ULONGLONG now = GetTickCount64();
        const bool unchanged = mode == lastMode_ && colors == lastColors_ &&
                               now - lastPublishTick_ < 1000;
        if (publisher_ && !unchanged) {
            publisher_->Publish(taskbar_, geometry.taskbar, geometry.horizontal, colors, mode);
            lastPublishTick_ = now;
        }
        lastColors_ = std::move(colors);
        lastMode_ = mode;

        if (showFallback && mode == chameleon::kSurfaceImage) {
            PaintFallback(geometry, taskbarWidth, taskbarHeight);
        } else {
            HideFallback();
        }
        return true;
    }

    void MaintainFallback(bool showFallback) {
        if (!showFallback || lastMode_ != chameleon::kSurfaceImage) {
            HideFallback();
            return;
        }
        RECT rect{};
        if (hwnd_ && GetWindowRect(taskbar_, &rect)) {
            Place(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
        }
    }

private:
    void PaintFallback(const Geometry& geometry, int width, int height) {
        if (!render_.Ensure(width, height)) {
            return;
        }
        auto* destination = static_cast<std::uint32_t*>(render_.bits());
        if (geometry.horizontal) {
            for (int y = 0; y < height; ++y) {
                std::copy(lastColors_.begin(), lastColors_.end(), destination + y * width);
            }
        } else {
            for (int y = 0; y < height; ++y) {
                std::fill_n(destination + y * width, width, lastColors_[static_cast<std::size_t>(y)]);
            }
        }
        Place(geometry.taskbar.left, geometry.taskbar.top, width, height);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Place(int left, int top, int width, int height) {
        SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
        SetWindowPos(hwnd_, taskbar_, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW |
                         SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Backdrop*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Backdrop*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (self) {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window, &paint);
                if (self->render_.dc()) {
                    BitBlt(dc, 0, 0, self->render_.width(), self->render_.height(),
                           self->render_.dc(), 0, 0, SRCCOPY);
                }
                EndPaint(window, &paint);
                return 0;
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    friend bool RegisterWindowClasses();

    HWND taskbar_ = nullptr;
    HWND hwnd_ = nullptr;
    chameleon::SurfacePublisher* publisher_ = nullptr;
    DibBuffer capture_;
    DibBuffer render_;
    chameleon::TemporalFilter temporal_;
    std::vector<std::uint32_t> lastColors_;
    chameleon::SurfaceMode lastMode_ = chameleon::kSurfaceNative;
    ULONGLONG lastPublishTick_ = 0;
    bool frozen_ = false;
};

// --- Application -------------------------------------------------------------

class Application final : public chameleon::SettingsHost {
public:
    explicit Application(bool testRun) : testRun_(testRun) {}

    bool Initialize() {
        settings_ = chameleon::LoadSettings();
        tuning_ = chameleon::TuningForSettings(settings_);
        legacyApplied_ = settings_.autoTransparency;
        chameleon::SaveSettings(settings_);
        surfacePublisher_.Initialize();
        PublishContrastSettings();

        controller_ = CreateWindowExW(0, kControllerClass, kApplicationName, WS_OVERLAPPED,
                                      0, 0, 0, 0, nullptr, nullptr, g_instance, this);
        if (!controller_) {
            return false;
        }
        settingsWindow_ = std::make_unique<chameleon::SettingsWindow>(g_instance, this);

        if (!testRun_) {
            chameleon::SharedObjectSecurity security;
            activeEvent_ = CreateEventW(security.get(), TRUE, TRUE, chameleon::kActiveEventName);
            if (!activeEvent_) {
                activeEvent_ = CreateEventW(nullptr, TRUE, TRUE, chameleon::kActiveEventName);
            }
            AttachBridge();
            if (!bridgeResult_.attached && bridgeResult_.retryable) {
                StartBridgeRetries();
            }
        }

        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        AddTrayIcon();
        DiscoverTaskbars();
        UpdateSamplingSchedule();
        SetTimer(controller_, kUiTimer, 100, nullptr);
        if (testRun_) {
            SetTimer(controller_, kTestExitTimer, 6000, nullptr);
        }
        lastTick_ = GetTickCount64();

        const bool startupLaunch = wcsstr(GetCommandLineW(), L"--startup") != nullptr;
        if (!testRun_ && !startupLaunch && !settings_.launchHidden) {
            settingsWindow_->Show();
        }
        return true;
    }

    int Run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    ~Application() override {
        Shutdown();
        settingsWindow_.reset();
        if (controller_) {
            DestroyWindow(controller_);
        }
        if (activeEvent_) {
            CloseHandle(activeEvent_);
        }
    }

    // --- SettingsHost ------------------------------------------------------------
    chameleon::Settings& EditableSettings() override { return settings_; }

    void SettingsEdited() override {
        settings_.preset = chameleon::MatchingPreset(settings_);
        ApplySettingsLive();
    }

    void ChoosePreset(chameleon::SamplingPreset preset) override {
        chameleon::ApplyPreset(settings_, preset);
        ApplySettingsLive();
        if (settingsWindow_) {
            settingsWindow_->Refresh();
        }
    }

    bool IsPaused() const override { return paused_; }

    void SetPaused(bool paused) override {
        if (paused_ == paused) {
            return;
        }
        paused_ = paused;
        if (paused_) {
            DeactivateBridge();
            RestoreTaskbars();
        } else {
            if (activeEvent_) {
                SetEvent(activeEvent_);
            }
            DiscoverTaskbars();
            ApplyLegacyTransparency();
        }
        UpdateTrayTip();
        if (settingsWindow_) {
            settingsWindow_->Refresh();
        }
    }

    bool StartsWithWindows() const override { return chameleon::IsRunAtStartupEnabled(); }

    void SetStartsWithWindows(bool enabled) override {
        if (!chameleon::SetRunAtStartupEnabled(enabled)) {
            MessageBoxW(settingsWindow_ ? settingsWindow_->Handle() : nullptr,
                        L"Windows didn't allow ChameleonBar to change its startup entry.",
                        kApplicationName, MB_OK | MB_ICONWARNING);
        }
    }

    chameleon::UiStatus Status() const override {
        using Level = chameleon::UiStatus::Level;
        chameleon::UiStatus status;
        if (paused_) {
            status.level = Level::Paused;
            status.headline = L"Paused";
            status.detail = L"Your taskbar is back to the Windows default.";
            return status;
        }
        if (explorerRestarting_) {
            status.level = Level::Waiting;
            status.headline = L"Restarting Explorer";
            status.detail = L"The taskbar will be back in a few seconds.";
            return status;
        }
        if (testRun_) {
            status.level = Level::Waiting;
            status.headline = L"Test run";
            status.detail = L"The bridge is not attached in test mode.";
            return status;
        }
        if (!bridgeResult_.attached) {
            if (bridgeRetries_ > 0) {
                status.level = Level::Waiting;
                status.headline = L"Connecting";
                status.detail = bridgeResult_.message + L".";
            } else {
                status.level = Level::Problem;
                status.headline = L"Needs attention";
                status.detail = bridgeResult_.message + L".";
            }
            return status;
        }
        const auto health = surfacePublisher_.Health();
        const ULONGLONG now = GetTickCount64();
        if (!health.alive) {
            if (now - bridgeAttachedAt_ < kBridgeGraceMilliseconds) {
                status.level = Level::Waiting;
                status.headline = L"Connecting";
                status.detail = L"Waiting for the taskbar to respond.";
            } else {
                status.level = Level::Problem;
                status.headline = L"Taskbar not responding";
                status.detail = L"Try Restart Explorer on the General page.";
            }
            return status;
        }
        if (health.outdated) {
            status.level = Level::Waiting;
            status.headline = L"Restart Explorer to finish";
            status.detail = L"Explorer is still running the previous bridge. Use Restart Explorer on the General page.";
            return status;
        }
        if (health.surfaces == 0 && publishingSince_ != 0 &&
            now - publishingSince_ > kBridgeGraceMilliseconds) {
            status.level = Level::Problem;
            status.headline = L"Can't reach the bar";
            status.detail = L"This Windows build hid the taskbar background. Restart Explorer once.";
            return status;
        }
        status.level = Level::Live;
        status.headline = L"Live";
        if (health.surfaces > 0) {
            status.detail = health.surfaces == 1
                ? L"Coloring your taskbar."
                : L"Coloring " + std::to_wstring(health.surfaces) + L" taskbars.";
        } else {
            status.detail = L"Ready.";
        }
        if (settings_.adaptiveTrayContrast && health.trays > 0) {
            status.detail += L" Tray contrast is adapting.";
        }
        return status;
    }

    bool Preview(chameleon::UiPreview& preview) const override {
        const Backdrop* chosen = nullptr;
        for (const auto& backdrop : backdrops_) {
            if (!backdrop->lastColors().empty() && (!chosen || backdrop->primary())) {
                chosen = backdrop.get();
            }
        }
        if (!chosen || paused_) {
            preview.pixels.clear();
            preview.clear = false;
            preview.frozen = false;
            return false;
        }
        preview.pixels = chosen->lastColors();
        preview.clear = chosen->lastMode() == chameleon::kSurfaceClear;
        preview.frozen = chosen->frozen();
        return true;
    }

    void Perform(chameleon::UiAction action) override {
        switch (action) {
        case chameleon::UiAction::RestartExplorer:
            ConfirmRestartExplorer();
            break;
        case chameleon::UiAction::OpenSettingsFolder:
            CreateDirectoryW(chameleon::SettingsDirectory().c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", chameleon::SettingsDirectory().c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
            break;
        case chameleon::UiAction::OpenSetupGuide:
            OpenSetupGuide();
            break;
        case chameleon::UiAction::ResetDefaults:
            if (MessageBoxW(settingsWindow_ ? settingsWindow_->Handle() : nullptr,
                            L"Reset every ChameleonBar setting to its default?",
                            kApplicationName, MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                settings_ = chameleon::Settings{};
                chameleon::ApplyPreset(settings_, chameleon::SamplingPreset::Seamless);
                ApplySettingsLive();
                if (settingsWindow_) {
                    settingsWindow_->Refresh();
                }
            }
            break;
        }
    }

private:
    // --- lifecycle ---------------------------------------------------------------
    void Shutdown() {
        if (shutDown_) {
            return;
        }
        shutDown_ = true;
        StopSamplingSchedule();
        SaveSettingsNow();
        DeactivateBridge();
        RestoreTaskbars();
        backdrops_.clear();  // releases every shared-memory slot
        RemoveTrayIcon();
    }

    // --- sampling schedule -------------------------------------------------------
    void StopSamplingSchedule() {
        if (controller_) {
            KillTimer(controller_, kSamplingTimer);
        }
        scheduledInterval_ = -1;
    }

    void UpdateSamplingSchedule() {
        if (!controller_ || scheduledInterval_ == tuning_.intervalMilliseconds) {
            return;
        }
        if (SetTimer(controller_, kSamplingTimer, static_cast<UINT>(tuning_.intervalMilliseconds), nullptr)) {
            scheduledInterval_ = tuning_.intervalMilliseconds;
        }
    }

    void QueueSettingsSave() {
        if (controller_) {
            SetTimer(controller_, kSettingsSaveTimer, 400, nullptr);
        }
    }

    void SaveSettingsNow() {
        if (controller_) {
            KillTimer(controller_, kSettingsSaveTimer);
        }
        chameleon::SaveSettings(settings_);
    }

    void PublishContrastSettings() {
        surfacePublisher_.SetContrast(settings_.adaptiveTrayContrast,
                                      settings_.lightGlyphThresholdPercent,
                                      settings_.darkGlyphThresholdPercent);
    }

    void ApplyLegacyTransparency() {
        for (const auto& backdrop : backdrops_) {
            SetTaskbarTransparent(backdrop->taskbar(), !paused_ && settings_.autoTransparency);
        }
    }

    void ApplySettingsLive() {
        tuning_ = chameleon::TuningForSettings(settings_);
        PublishContrastSettings();
        QueueSettingsSave();
        UpdateSamplingSchedule();
        if (legacyApplied_ != settings_.autoTransparency) {
            legacyApplied_ = settings_.autoTransparency;
            ApplyLegacyTransparency();
        }
    }

    // --- bridge ------------------------------------------------------------------
    void AttachBridge() {
        if (!activeEvent_) {
            bridgeResult_ = {};
            bridgeResult_.status = HRESULT_FROM_WIN32(GetLastError());
            bridgeResult_.message = L"ChameleonBar couldn't create its fail-safe event";
            return;
        }
        const bool wasAttached = bridgeResult_.attached;
        bridgeResult_ = chameleon::AttachLocalXamlBridge();
        if (bridgeResult_.attached && !wasAttached) {
            bridgeAttachedAt_ = GetTickCount64();
        }
        wchar_t statusCode[16]{};
        swprintf_s(statusCode, L"0x%08lX", static_cast<unsigned long>(bridgeResult_.status));
        const std::wstring path = chameleon::SettingsPath();
        WritePrivateProfileStringW(L"diagnostics", L"bridge_status", statusCode, path.c_str());
        WritePrivateProfileStringW(L"diagnostics", L"bridge_message", bridgeResult_.message.c_str(), path.c_str());
    }

    void StartBridgeRetries() {
        bridgeRetries_ = kBridgeRetryLimit;
        SetTimer(controller_, kBridgeRetryTimer, 2000, nullptr);
    }

    void RetryBridge() {
        if (paused_) {
            return;
        }
        AttachBridge();
        if (bridgeResult_.attached || !bridgeResult_.retryable || --bridgeRetries_ <= 0) {
            bridgeRetries_ = 0;
            KillTimer(controller_, kBridgeRetryTimer);
        }
    }

    // Every 10 s, write how regularly the bridge pulsed to the [diagnostics]
    // section of settings.ini so stalls can be diagnosed after the fact.
    void RecordBridgeTiming() {
        const ULONGLONG now = GetTickCount64();
        if (timingWindowStart_ == 0) {
            timingWindowStart_ = now;
            surfacePublisher_.TakeTiming();
            return;
        }
        if (now - timingWindowStart_ < 10000) {
            return;
        }
        const ULONGLONG elapsed = now - timingWindowStart_;
        timingWindowStart_ = now;
        const auto timing = surfacePublisher_.TakeTiming();
        const auto health = surfacePublisher_.Health();
        const long pulses = static_cast<long>(timing.pulses - lastPulseCount_);
        lastPulseCount_ = timing.pulses;
        worstGapMs_ = std::max<long>(worstGapMs_, timing.maxGapMs);
        if (paused_ || !bridgeResult_.attached) {
            return;
        }
        const std::wstring path = chameleon::SettingsPath();
        const auto write = [&](const wchar_t* key, long long value) {
            WritePrivateProfileStringW(L"diagnostics", key, std::to_wstring(value).c_str(), path.c_str());
        };
        write(L"bridge_pulses_per_second", pulses * 1000LL / static_cast<long long>(elapsed));
        write(L"bridge_pulse_gap_max_ms", timing.maxGapMs);
        write(L"bridge_pulse_gap_worst_ms", worstGapMs_);
        write(L"bridge_slowest_pulse_ms", timing.slowestPulseMs);
        write(L"bridge_heartbeat_age_ms", health.heartbeatAge);
    }

    bool BridgeWorking() const {
        if (!bridgeResult_.attached) {
            return false;
        }
        return surfacePublisher_.Health().alive ||
               GetTickCount64() - bridgeAttachedAt_ < kBridgeGraceMilliseconds;
    }

    void DeactivateBridge() {
        if (activeEvent_) {
            ResetEvent(activeEvent_);
        }
    }

    void ConfirmRestartExplorer() {
        if (explorerRestarting_) {
            return;
        }
        if (MessageBoxW(settingsWindow_ ? settingsWindow_->Handle() : nullptr,
                        L"Restart Windows Explorer?\n\nThe taskbar and desktop reload in a few "
                        L"seconds and open File Explorer windows close. ChameleonBar reconnects "
                        L"automatically.",
                        kApplicationName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            return;
        }
        explorerRestarting_ = true;
        bridgeResult_ = {};
        bridgeResult_.message = L"Restarting Explorer";
        HWND controller = controller_;
        std::thread([controller] {
            chameleon::RestartExplorer();
            PostMessageW(controller, kExplorerRestartedMessage, 0, 0);
        }).detach();
    }

    // --- tray --------------------------------------------------------------------
    void AddTrayIcon() {
        trayIcon_ = {};
        trayIcon_.cbSize = sizeof(trayIcon_);
        trayIcon_.hWnd = controller_;
        trayIcon_.uID = 1;
        trayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        trayIcon_.uCallbackMessage = kTrayMessage;
        trayIcon_.hIcon = LoadApplicationIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        wcscpy_s(trayIcon_.szTip, paused_ ? L"ChameleonBar (paused)" : L"ChameleonBar");
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &trayIcon_) != FALSE;
        trayIcon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &trayIcon_);
    }

    void UpdateTrayTip() {
        if (!trayAdded_) {
            return;
        }
        trayIcon_.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcscpy_s(trayIcon_.szTip, paused_ ? L"ChameleonBar (paused)" : L"ChameleonBar");
        Shell_NotifyIconW(NIM_MODIFY, &trayIcon_);
    }

    void RemoveTrayIcon() {
        if (trayAdded_) {
            Shell_NotifyIconW(NIM_DELETE, &trayIcon_);
            trayAdded_ = false;
        }
    }

    void ShowTrayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuOpenWindow, L"Open ChameleonBar");
        SetMenuDefaultItem(menu, kMenuOpenWindow, FALSE);
        AppendMenuW(menu, MF_STRING | (paused_ ? MF_CHECKED : 0), kMenuPause, L"Pause color matching");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU presetMenu = CreatePopupMenu();
        const auto presets = chameleon::Presets();
        for (std::size_t i = 0; i < presets.size(); ++i) {
            AppendMenuW(presetMenu,
                        MF_STRING | (settings_.preset == presets[i].id ? MF_CHECKED : 0),
                        kMenuPresetBase + static_cast<UINT>(i), presets[i].name);
        }
        if (settings_.preset == chameleon::SamplingPreset::Custom) {
            AppendMenuW(presetMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(presetMenu, MF_STRING | MF_CHECKED | MF_GRAYED, 0, L"Custom");
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(presetMenu), L"Preset");
        AppendMenuW(menu, MF_STRING | (chameleon::IsRunAtStartupEnabled() ? MF_CHECKED : 0),
                    kMenuRunAtStartup, L"Start with Windows");
        AppendMenuW(menu, MF_STRING, kMenuRestartExplorer, L"Restart Explorer...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit and restore taskbar");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(controller_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, controller_, nullptr);
        PostMessageW(controller_, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    void HandleMenuCommand(UINT command) {
        const auto presets = chameleon::Presets();
        if (command >= kMenuPresetBase && command < kMenuPresetBase + presets.size()) {
            ChoosePreset(presets[command - kMenuPresetBase].id);
            return;
        }
        switch (command) {
        case kMenuOpenWindow:
            settingsWindow_->Show();
            break;
        case kMenuPause:
            SetPaused(!paused_);
            break;
        case kMenuRunAtStartup:
            SetStartsWithWindows(!chameleon::IsRunAtStartupEnabled());
            if (settingsWindow_) {
                settingsWindow_->Refresh();
            }
            break;
        case kMenuRestartExplorer:
            ConfirmRestartExplorer();
            break;
        case kMenuExit:
            DestroyWindow(controller_);
            break;
        default:
            break;
        }
    }

    void OpenSetupGuide() {
        const std::wstring guide = ApplicationDirectory() + L"\\Setup and Troubleshooting.txt";
        const DWORD attributes = GetFileAttributesW(guide.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ShellExecuteW(nullptr, L"open", guide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        MessageBoxW(settingsWindow_ ? settingsWindow_->Handle() : nullptr,
                    L"\"Setup and Troubleshooting.txt\" isn't next to ChameleonBar.exe. "
                    L"Re-extract the full download to get it back.",
                    kApplicationName, MB_OK | MB_ICONINFORMATION);
    }

    // --- taskbars ----------------------------------------------------------------
    void RestoreTaskbars() {
        for (const auto& backdrop : backdrops_) {
            backdrop->Restore();
        }
        for (HWND taskbar : EnumerateTaskbars()) {
            if (settings_.autoTransparency || legacyApplied_) {
                SetTaskbarTransparent(taskbar, false);
            }
        }
        publishingSince_ = 0;
    }

    void DiscoverTaskbars() {
        const auto taskbars = EnumerateTaskbars();
        const std::unordered_set<HWND> live(taskbars.begin(), taskbars.end());
        std::erase_if(backdrops_, [&live](const std::unique_ptr<Backdrop>& backdrop) {
            return !backdrop->valid() || !live.contains(backdrop->taskbar());
        });
        for (HWND taskbar : taskbars) {
            const bool known = std::any_of(
                backdrops_.begin(), backdrops_.end(),
                [taskbar](const std::unique_ptr<Backdrop>& backdrop) { return backdrop->taskbar() == taskbar; });
            if (!known) {
                backdrops_.push_back(std::make_unique<Backdrop>(taskbar, &surfacePublisher_));
                if (!paused_ && settings_.autoTransparency) {
                    SetTaskbarTransparent(taskbar, true);
                }
            }
        }
        lastDiscovery_ = GetTickCount64();
    }

    void Sample() {
        const ULONGLONG now = GetTickCount64();
        if (now - lastDiscovery_ >= kRediscoveryMilliseconds) {
            DiscoverTaskbars();
        }
        const double elapsed = lastTick_ == 0
            ? static_cast<double>(tuning_.intervalMilliseconds)
            : static_cast<double>(now - lastTick_);
        lastTick_ = now;
        if (paused_ || explorerRestarting_) {
            return;
        }

        RefreshWindowActivity(now);
        const bool showFallback = !testRun_ && !BridgeWorking();
        bool published = false;
        for (auto& backdrop : backdrops_) {
            if (settings_.freezeOnTaskbarActivity && HasTaskbarTransient(backdrop->taskbar())) {
                // Keep whatever is showing; popups must not feed back into it.
                backdrop->SetFrozen(true);
                backdrop->MaintainFallback(showFallback);
                published = published || backdrop->lastMode() != chameleon::kSurfaceNative;
                continue;
            }
            const bool clear = settings_.transparentOnDesktop &&
                               !HasVisibleApplication(backdrop->taskbar());
            if (backdrop->Update(tuning_, elapsed, showFallback, visibleApplicationRects_,
                                 clear ? chameleon::kSurfaceClear : chameleon::kSurfaceImage)) {
                published = true;
            }
        }
        if (published && publishingSince_ == 0) {
            publishingSince_ = now;
        } else if (!published) {
            publishingSince_ = 0;
        }
    }

    void RefreshWindowActivity(ULONGLONG now) {
        if (lastWindowActivityScan_ != 0 &&
            now - lastWindowActivityScan_ < kWindowActivityScanMilliseconds) {
            return;
        }
        lastWindowActivityScan_ = now;
        auto activity = chameleon::EnumerateWindowActivity();
        visibleApplicationRects_ = std::move(activity.applicationWindows);
        transientWindowRects_ = std::move(activity.transientWindows);
    }

    bool HasTaskbarTransient(HWND taskbar) const {
        RECT taskbarRect{};
        MONITORINFO information{};
        information.cbSize = sizeof(information);
        const HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
        if (!GetWindowRect(taskbar, &taskbarRect) || !monitor ||
            !GetMonitorInfoW(monitor, &information)) {
            return false;
        }
        return std::any_of(
            transientWindowRects_.begin(), transientWindowRects_.end(),
            [&](const RECT& rectangle) {
                return chameleon::IsNearTaskbar(rectangle, taskbarRect, information.rcMonitor);
            });
    }

    bool HasVisibleApplication(HWND taskbar) const {
        const HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
        MONITORINFO information{};
        information.cbSize = sizeof(information);
        if (!monitor || !GetMonitorInfoW(monitor, &information)) {
            return !visibleApplicationRects_.empty();
        }
        return std::any_of(
            visibleApplicationRects_.begin(), visibleApplicationRects_.end(),
            [&](const RECT& rectangle) {
                return chameleon::SubstantiallyIntersects(rectangle, information.rcMonitor);
            });
    }

    // --- window procedure --------------------------------------------------------
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        if (self->taskbarCreatedMessage_ != 0 && message == self->taskbarCreatedMessage_) {
            // Explorer (re)started: new taskbars, new tray, and the bridge
            // must be loaded into the new process.
            self->RemoveTrayIcon();
            self->AddTrayIcon();
            self->DiscoverTaskbars();
            self->ApplyLegacyTransparency();
            if (!self->testRun_) {
                self->bridgeResult_ = {};
                self->bridgeResult_.message = L"Reconnecting to the new taskbar";
                self->StartBridgeRetries();
            }
            return 0;
        }

        switch (message) {
        case kShowSettingsMessage:
            self->settingsWindow_->Show();
            return 0;
        case kExplorerRestartedMessage:
            self->explorerRestarting_ = false;
            if (!self->testRun_ && !self->bridgeResult_.attached && self->bridgeRetries_ == 0) {
                self->StartBridgeRetries();
            }
            return 0;
        case WM_TIMER:
            switch (wParam) {
            case kSamplingTimer:
                self->Sample();
                break;
            case kSettingsSaveTimer:
                self->SaveSettingsNow();
                break;
            case kUiTimer:
                if (self->settingsWindow_) {
                    self->settingsWindow_->Tick();
                }
                self->RecordBridgeTiming();
                break;
            case kBridgeRetryTimer:
                self->RetryBridge();
                break;
            case kTestExitTimer:
                DestroyWindow(window);
                break;
            default:
                break;
            }
            return 0;
        case WM_COMMAND:
            self->HandleMenuCommand(LOWORD(wParam));
            return 0;
        case kTrayMessage:
            switch (LOWORD(lParam)) {
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                self->ShowTrayMenu();
                break;
            case NIN_SELECT:
            case NIN_KEYSELECT:
            case WM_LBUTTONDBLCLK:
                self->settingsWindow_->Show();
                break;
            default:
                break;
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            self->DiscoverTaskbars();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) {
                self->Shutdown();
            }
            return 0;
        case WM_DESTROY:
            self->Shutdown();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    friend bool RegisterWindowClasses();

    HWND controller_ = nullptr;
    std::unique_ptr<chameleon::SettingsWindow> settingsWindow_;
    UINT taskbarCreatedMessage_ = 0;
    NOTIFYICONDATAW trayIcon_{};
    bool trayAdded_ = false;
    bool paused_ = false;
    bool shutDown_ = false;
    bool legacyApplied_ = false;
    bool testRun_ = false;
    chameleon::Settings settings_{};
    chameleon::RuntimeTuning tuning_ = chameleon::TuningForSettings(chameleon::Settings{});
    chameleon::SurfacePublisher surfacePublisher_;
    std::vector<std::unique_ptr<Backdrop>> backdrops_;
    ULONGLONG lastTick_ = 0;
    ULONGLONG lastDiscovery_ = 0;
    ULONGLONG lastWindowActivityScan_ = 0;
    ULONGLONG publishingSince_ = 0;
    std::vector<RECT> visibleApplicationRects_;
    std::vector<RECT> transientWindowRects_;
    int scheduledInterval_ = -1;
    HANDLE activeEvent_ = nullptr;
    chameleon::BridgeAttachResult bridgeResult_{};
    ULONGLONG bridgeAttachedAt_ = 0;
    int bridgeRetries_ = 0;
    std::atomic_bool explorerRestarting_{false};
    ULONGLONG timingWindowStart_ = 0;
    LONG lastPulseCount_ = 0;
    long worstGapMs_ = 0;
};

bool RegisterWindowClasses() {
    WNDCLASSEXW controller{};
    controller.cbSize = sizeof(controller);
    controller.hInstance = g_instance;
    controller.lpfnWndProc = Application::WindowProcedure;
    controller.lpszClassName = kControllerClass;
    controller.hIcon = LoadApplicationIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    controller.hIconSm = LoadApplicationIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (!RegisterClassExW(&controller) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW backdrop{};
    backdrop.cbSize = sizeof(backdrop);
    backdrop.hInstance = g_instance;
    backdrop.lpfnWndProc = Backdrop::WindowProcedure;
    backdrop.lpszClassName = kBackdropClass;
    backdrop.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&backdrop) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    return chameleon::SettingsWindow::Register(g_instance);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    using SetDpiAwarenessFunction = BOOL(WINAPI*)(HANDLE);
    const auto setDpiAwareness = reinterpret_cast<SetDpiAwarenessFunction>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")));
    if (setDpiAwareness) {
        setDpiAwareness(reinterpret_cast<HANDLE>(-4));  // Per-monitor aware v2.
    } else {
        SetProcessDPIAware();
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ChameleonBar.SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kControllerClass, nullptr)) {
            PostMessageW(existing, kShowSettingsMessage, 0, 0);
        }
        if (mutex) {
            CloseHandle(mutex);
        }
        return 0;
    }

    if (!chameleon::StartGraphics()) {
        MessageBoxW(nullptr, L"ChameleonBar couldn't start Windows graphics (GDI+).",
                    kApplicationName, MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }
    if (!RegisterWindowClasses()) {
        MessageBoxW(nullptr, L"ChameleonBar couldn't register its windows.", kApplicationName,
                    MB_OK | MB_ICONERROR);
        chameleon::StopGraphics();
        CloseHandle(mutex);
        return 1;
    }

    int result = 0;
    {
        const bool testRun = wcsstr(GetCommandLineW(), L"--test-run") != nullptr;
        Application application(testRun);
        if (!application.Initialize()) {
            MessageBoxW(nullptr, L"ChameleonBar couldn't start.", kApplicationName, MB_OK | MB_ICONERROR);
            result = 1;
        } else {
            result = application.Run();
        }
    }
    chameleon::StopGraphics();
    CloseHandle(mutex);
    return result;
}
