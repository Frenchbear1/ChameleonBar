#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "color_sampler.h"
#include "resource.h"
#include "settings.h"
#include "surface_publisher.h"
#include "window_activity.h"
#include "xaml_bridge_client.h"

namespace {

constexpr wchar_t kApplicationName[] = L"ChameleonBar";
constexpr wchar_t kControllerClass[] = L"ChameleonBar.Controller";
constexpr wchar_t kBackdropClass[] = L"ChameleonBar.Backdrop";
constexpr UINT kTrayMessage = WM_APP + 41;
constexpr UINT kShowSettingsMessage = WM_APP + 42;
constexpr UINT_PTR kSamplingTimer = 1;
constexpr UINT_PTR kTestExitTimer = 2;
constexpr UINT_PTR kSettingsSaveTimer = 3;

constexpr UINT kMenuPause = 1001;
constexpr UINT kMenuOpenWindow = 1000;
constexpr UINT kMenuPresetExact = 1101;
constexpr UINT kMenuPresetBalanced = 1102;
constexpr UINT kMenuPresetCalm = 1103;
constexpr UINT kMenuPresetCustom = 1104;
constexpr UINT kMenuAutoTransparency = 1201;
constexpr UINT kMenuRunAtStartup = 1301;
constexpr UINT kMenuOpenSettings = 1401;
constexpr UINT kMenuOpenSetup = 1402;
constexpr UINT kMenuAbout = 1501;
constexpr UINT kMenuExit = 1601;

constexpr UINT kControlPreset = 2001;
constexpr UINT kControlHide = 2003;
constexpr UINT kControlReset = 2004;
constexpr UINT kControlOpenSetup = 2005;
constexpr UINT kControlAdaptiveContrast = 2006;
constexpr UINT kControlStartWithWindows = 2007;
constexpr UINT kControlLaunchHidden = 2008;
constexpr UINT kControlLegacyTransparency = 2009;
constexpr UINT kControlBackgroundOnly = 2010;
constexpr UINT kControlTransparentOnDesktop = 2014;
constexpr UINT kControlFreezeOnTaskbarActivity = 2015;
constexpr UINT kControlPageAppearance = 2011;
constexpr UINT kControlPageSampling = 2012;
constexpr UINT kControlPageContrast = 2013;

constexpr UINT kSliderBlurWidth = 2101;
constexpr UINT kSliderBlurStrength = 2102;
constexpr UINT kSliderTransition = 2103;
constexpr UINT kSliderSaturation = 2104;
constexpr UINT kSliderBrightness = 2105;
constexpr UINT kSliderSampleDepth = 2111;
constexpr UINT kSliderSampleOffset = 2112;
constexpr UINT kSliderCellWidth = 2113;
constexpr UINT kSliderEdgeProtection = 2114;
constexpr UINT kSliderUpdateInterval = 2115;
constexpr UINT kSliderDetailRejection = 2116;
constexpr UINT kSliderLightThreshold = 2121;
constexpr UINT kSliderDarkThreshold = 2122;
constexpr UINT kImmediateSamplingMessage = WM_APP + 59;

constexpr int kRediscoveryMilliseconds = 1800;
constexpr int kWindowActivityScanMilliseconds = 48;

HINSTANCE g_instance = nullptr;

constexpr COLORREF kUiBackground = RGB(15, 17, 22);
constexpr COLORREF kUiSurface = RGB(25, 28, 36);
constexpr COLORREF kUiSurfaceRaised = RGB(36, 40, 50);
constexpr COLORREF kUiText = RGB(242, 244, 248);
constexpr COLORREF kUiMutedText = RGB(164, 172, 188);
constexpr COLORREF kUiAccent = RGB(92, 224, 190);
constexpr COLORREF kUiAccentPressed = RGB(67, 190, 157);

void DrawRoundedFill(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

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
        g_instance,
        MAKEINTRESOURCEW(IDI_CHAMELEONBAR),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR | LR_SHARED));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

enum class AccentState : int {
    Disabled = 0,
    EnableGradient = 1,
    EnableTransparentGradient = 2,
    EnableBlurBehind = 3,
    EnableAcrylicBlurBehind = 4,
};

struct AccentPolicy {
    AccentState state;
    DWORD flags;
    DWORD gradientColor;
    DWORD animationId;
};

enum class WindowCompositionAttribute : int {
    AccentPolicy = 19,
};

struct WindowCompositionAttributeData {
    WindowCompositionAttribute attribute;
    PVOID data;
    SIZE_T sizeOfData;
};

using SetWindowCompositionAttributeFunction = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

void SetTaskbarTransparent(HWND taskbar, bool transparent) {
    static const auto function = reinterpret_cast<SetWindowCompositionAttributeFunction>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!function || !IsWindow(taskbar)) {
        return;
    }

    AccentPolicy policy{};
    policy.state = transparent ? AccentState::EnableTransparentGradient : AccentState::Disabled;
    policy.flags = transparent ? 2u : 0u;
    policy.gradientColor = 0u;
    WindowCompositionAttributeData data{
        WindowCompositionAttribute::AccentPolicy,
        &policy,
        sizeof(policy),
    };
    function(taskbar, &data);
    RedrawWindow(taskbar, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

bool IsTaskbarWindow(HWND window) {
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return wcscmp(className, L"Shell_TrayWnd") == 0 ||
           wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

std::vector<HWND> EnumerateTaskbars() {
    std::vector<HWND> result;
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto* windows = reinterpret_cast<std::vector<HWND>*>(parameter);
            if (IsTaskbarWindow(window)) {
                windows->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

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
        width_ = 0;
        height_ = 0;
        stride_ = 0;
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

struct Geometry {
    RECT taskbar{};
    RECT sample{};
    chameleon::AdjacentEdge adjacentEdge = chameleon::AdjacentEdge::Bottom;
    bool horizontal = true;
};

bool ResolveGeometry(
    HWND taskbar,
    int requestedDepth,
    int requestedOffset,
    Geometry& geometry) {
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

    return geometry.sample.right > geometry.sample.left && geometry.sample.bottom > geometry.sample.top;
}

std::vector<int> ApplicationBoundariesForSample(
    const Geometry& geometry,
    const std::vector<RECT>& applicationWindows) {
    std::vector<int> boundaries;
    for (const RECT& application : applicationWindows) {
        RECT intersection{};
        if (!IntersectRect(&intersection, &geometry.sample, &application)) {
            continue;
        }
        if (geometry.horizontal) {
            boundaries.push_back(std::clamp(
                static_cast<int>(application.left - geometry.sample.left),
                0,
                static_cast<int>(geometry.sample.right - geometry.sample.left)));
            boundaries.push_back(std::clamp(
                static_cast<int>(application.right - geometry.sample.left),
                0,
                static_cast<int>(geometry.sample.right - geometry.sample.left)));
        } else {
            boundaries.push_back(std::clamp(
                static_cast<int>(application.top - geometry.sample.top),
                0,
                static_cast<int>(geometry.sample.bottom - geometry.sample.top)));
            boundaries.push_back(std::clamp(
                static_cast<int>(application.bottom - geometry.sample.top),
                0,
                static_cast<int>(geometry.sample.bottom - geometry.sample.top)));
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(
        std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

class Backdrop {
public:
    Backdrop(HWND taskbar, chameleon::SurfacePublisher* publisher)
        : taskbar_(taskbar), publisher_(publisher) {
        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                WS_EX_TOPMOST,
            kBackdropClass,
            L"ChameleonBar backdrop",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            g_instance,
            this);
        if (hwnd_) {
            const BOOL disableTransitions = TRUE;
            DwmSetWindowAttribute(
                hwnd_,
                DWMWA_TRANSITIONS_FORCEDISABLED,
                &disableTransitions,
                sizeof(disableTransitions));
            const BOOL excludeFromPeek = TRUE;
            DwmSetWindowAttribute(
                hwnd_,
                DWMWA_EXCLUDED_FROM_PEEK,
                &excludeFromPeek,
                sizeof(excludeFromPeek));
        }
    }

    ~Backdrop() {
        if (publisher_) {
            publisher_->Hide(taskbar_);
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
        }
    }

    HWND taskbar() const { return taskbar_; }
    bool valid() const { return IsWindow(taskbar_) && IsWindow(hwnd_); }

    void Hide() {
        if (publisher_) {
            publisher_->Hide(taskbar_);
        }
        if (hwnd_) {
            ShowWindow(hwnd_, SW_HIDE);
        }
        temporal_.Reset();
    }

    bool Update(
        const chameleon::RuntimeTuning& tuning,
        double elapsedMilliseconds,
        bool showFallback,
        const std::vector<RECT>& applicationWindows) {
        Geometry geometry;
        if (!ResolveGeometry(
                taskbar_, tuning.sampleDepth, tuning.sampleOffset, geometry)) {
            Hide();
            return false;
        }

        const int sampleWidth = geometry.sample.right - geometry.sample.left;
        const int sampleHeight = geometry.sample.bottom - geometry.sample.top;
        if (!capture_.Ensure(sampleWidth, sampleHeight)) {
            return false;
        }

        HDC screen = GetDC(nullptr);
        const BOOL copied = BitBlt(
            capture_.dc(),
            0,
            0,
            sampleWidth,
            sampleHeight,
            screen,
            geometry.sample.left,
            geometry.sample.top,
            SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, screen);
        if (!copied) {
            return false;
        }

        chameleon::SamplerConfig samplerConfig;
        samplerConfig.cellWidth = tuning.cellWidth;
        samplerConfig.edgeThreshold = tuning.edgeThreshold;
        samplerConfig.horizontalBlurPixels = tuning.horizontalBlurPixels;
        samplerConfig.blurStrength = tuning.blurStrength;
        samplerConfig.backgroundOnly = tuning.backgroundOnly;
        samplerConfig.detailRejectionPixels = tuning.detailRejectionPixels;
        samplerConfig.protectedBoundaryPixels =
            ApplicationBoundariesForSample(geometry, applicationWindows);
        auto target = chameleon::ExtractColorField(
            static_cast<const std::uint8_t*>(capture_.bits()),
            sampleWidth,
            sampleHeight,
            capture_.stride(),
            geometry.adjacentEdge,
            samplerConfig);
        if (target.empty()) {
            return false;
        }
        chameleon::AdjustColorField(
            target, tuning.saturation, tuning.brightness);

        const auto& filtered = temporal_.Update(target, elapsedMilliseconds, tuning.transitionMilliseconds);
        const int taskbarWidth = geometry.taskbar.right - geometry.taskbar.left;
        const int taskbarHeight = geometry.taskbar.bottom - geometry.taskbar.top;
        const int outputLength = geometry.horizontal ? taskbarWidth : taskbarHeight;
        const auto colors = chameleon::ExpandColorField(
            filtered,
            outputLength,
            tuning.cellWidth,
            tuning.edgeThreshold,
            tuning.horizontalBlurPixels > 0 && tuning.blurStrength > 0.0f);
        if (colors.empty() || !render_.Ensure(taskbarWidth, taskbarHeight)) {
            return false;
        }
        if (publisher_) {
            publisher_->Publish(
                taskbar_, geometry.taskbar, geometry.horizontal, colors);
        }

        auto* destination = static_cast<std::uint32_t*>(render_.bits());
        if (geometry.horizontal) {
            for (int y = 0; y < taskbarHeight; ++y) {
                std::copy(colors.begin(), colors.end(), destination + y * taskbarWidth);
            }
        } else {
            for (int y = 0; y < taskbarHeight; ++y) {
                std::fill_n(destination + y * taskbarWidth, taskbarWidth, colors[static_cast<std::size_t>(y)]);
            }
        }

        if (showFallback) {
            MaintainPlacement(
                geometry.taskbar.left,
                geometry.taskbar.top,
                taskbarWidth,
                taskbarHeight);
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        } else {
            ShowWindow(hwnd_, SW_HIDE);
        }
        return true;
    }

    void MaintainPlacement(bool showFallback) {
        if (!showFallback) {
            ShowWindow(hwnd_, SW_HIDE);
            return;
        }
        RECT taskbarRect{};
        if (hwnd_ && GetWindowRect(taskbar_, &taskbarRect)) {
            MaintainPlacement(
                taskbarRect.left,
                taskbarRect.top,
                taskbarRect.right - taskbarRect.left,
                taskbarRect.bottom - taskbarRect.top);
        }
    }

private:
    void MaintainPlacement(int left, int top, int width, int height) {
        // Top-level fallback for systems where the local XAML surface bridge is
        // unavailable. The bridge itself paints the same pixels directly into
        // the native taskbar and is therefore immune to shell window bands.
        SetWindowPos(
            hwnd_,
            HWND_TOPMOST,
            left,
            top,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING |
                SWP_NOOWNERZORDER);
        SetWindowPos(
            hwnd_,
            taskbar_,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW |
                SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Backdrop* self = reinterpret_cast<Backdrop*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Backdrop*>(create->lpCreateParams);
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
                    BitBlt(
                        dc,
                        0,
                        0,
                        self->render_.width(),
                        self->render_.height(),
                        self->render_.dc(),
                        0,
                        0,
                        SRCCOPY);
                }
                EndPaint(window, &paint);
                return 0;
            }
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
};

class Application {
public:
    explicit Application(bool testRun) : testRun_(testRun) {}

    bool Initialize() {
        if (testRun_) {
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
            testLogPath_ = executable;
            const std::size_t slash = testLogPath_.find_last_of(L"\\/");
            testLogPath_ = testLogPath_.substr(0, slash + 1) + L"ChameleonBar-test.log";
            HANDLE file = CreateFileW(testLogPath_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
            }
            TestLog(L"test run initialized");
        }
        settings_ = chameleon::LoadSettings();
        tuning_ = chameleon::TuningForSettings(settings_);
        chameleon::SaveSettings(settings_);
        surfacePublisher_.Initialize();

        window_ = CreateWindowExW(
            0,
            kControllerClass,
            L"ChameleonBar settings",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            620,
            720,
            nullptr,
            nullptr,
            g_instance,
            this);
        if (!window_) {
            return false;
        }

        BuildSettingsWindow();
        PopulateSettingsControls();

        if (!testRun_) {
            activeEvent_ = CreateEventW(
                nullptr, TRUE, TRUE, chameleon::kActiveEventName);
            AttachBridge();
        }

        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        AddTrayIcon();
        DiscoverTaskbars();
        UpdateSamplingSchedule();
        if (testRun_) {
            SetTimer(window_, kTestExitTimer, 6000, nullptr);
        }
        lastTick_ = GetTickCount64();

        if (!testRun_ && settings_.showFirstRunHelp) {
            settings_.showFirstRunHelp = false;
            chameleon::SaveSettings(settings_);
            ShowFirstRunHelp();
        }
        const bool startupLaunch = wcsstr(GetCommandLineW(), L"--startup") != nullptr;
        if (!testRun_ && !startupLaunch && !settings_.launchHidden) {
            ShowSettingsWindow();
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

    ~Application() {
        StopSamplingSchedule();
        DeactivateBridge();
        RestoreTaskbars();
        RemoveTrayIcon();
        if (window_) {
            DestroyWindow(window_);
        }
        if (titleFont_) {
            DeleteObject(titleFont_);
        }
        if (uiFont_) {
            DeleteObject(uiFont_);
        }
        if (backgroundBrush_) {
            DeleteObject(backgroundBrush_);
        }
        if (surfaceBrush_) {
            DeleteObject(surfaceBrush_);
        }
        if (activeEvent_) {
            CloseHandle(activeEvent_);
        }
    }

private:
    struct SliderBinding {
        UINT id = 0;
        HWND slider = nullptr;
        HWND valueLabel = nullptr;
        int page = 0;
        int chameleon::Settings::* member = nullptr;
        std::wstring suffix;
    };

    HWND CreateUiControl(
        const wchar_t* className,
        const wchar_t* text,
        DWORD style,
        int x,
        int y,
        int width,
        int height,
        UINT id,
        int page = -1,
        HFONT font = nullptr) {
        HWND control = CreateWindowExW(
            0,
            className,
            text,
            WS_CHILD | style,
            x,
            y,
            width,
            height,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            g_instance,
            nullptr);
        if (control) {
            if (wcscmp(className, WC_COMBOBOXW) == 0) {
                SetWindowTheme(control, L"DarkMode_CFD", nullptr);
            } else if (wcscmp(className, TRACKBAR_CLASSW) == 0 ||
                       wcscmp(className, WC_TABCONTROLW) == 0 ||
                       wcscmp(className, L"BUTTON") == 0) {
                SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
            }
            SendMessageW(
                control,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(font ? font : uiFont_),
                TRUE);
            if (page >= 0 && page < static_cast<int>(pageControls_.size())) {
                pageControls_[static_cast<std::size_t>(page)].push_back(control);
            } else {
                ShowWindow(control, SW_SHOW);
            }
        }
        return control;
    }

    void AddSlider(
        int page,
        UINT id,
        const wchar_t* label,
        int minimum,
        int maximum,
        int chameleon::Settings::* member,
        const wchar_t* suffix,
        int row,
        int top = 144) {
        const int y = top + row * 58;
        CreateUiControl(
            L"STATIC", label, SS_LEFT, 40, y, 410, 24, 0, page);
        HWND valueLabel = CreateUiControl(
            L"STATIC", L"", SS_RIGHT, 466, y, 104, 24, 0, page);
        HWND slider = CreateUiControl(
            TRACKBAR_CLASSW,
            L"",
            TBS_HORZ | TBS_NOTICKS,
            36,
            y + 27,
            534,
            24,
            id,
            page);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(minimum, maximum));
        SendMessageW(slider, TBM_SETLINESIZE, 0, 1);
        SendMessageW(slider, TBM_SETPAGESIZE, 0, std::max(1, (maximum - minimum) / 10));
        sliders_.push_back({id, slider, valueLabel, page, member, suffix});
    }

    void BuildSettingsWindow() {
        backgroundBrush_ = CreateSolidBrush(kUiBackground);
        surfaceBrush_ = CreateSolidBrush(kUiSurface);
        const BOOL darkMode = TRUE;
        DwmSetWindowAttribute(window_, 20, &darkMode, sizeof(darkMode));
        const int cornerPreference = 2;
        DwmSetWindowAttribute(
            window_, 33, &cornerPreference, sizeof(cornerPreference));
        SetWindowTheme(window_, L"DarkMode_Explorer", nullptr);

        uiFont_ = CreateFontW(
            -17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
        titleFont_ = CreateFontW(
            -30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");

        CreateUiControl(
            L"STATIC", L"ChameleonBar", SS_LEFT, 28, 18, 330, 38, 0, -1,
            titleFont_);
        CreateUiControl(
            L"STATIC",
            L"Live colors, matched locally across every taskbar.",
            SS_LEFT,
            30,
            56,
            380,
            24,
            0);
        CreateUiControl(
            L"STATIC", L"PROFILE", SS_LEFT, 430, 17, 80, 20, 0);
        presetCombo_ = CreateUiControl(
            L"BUTTON",
            L"Exact edges",
            BS_OWNERDRAW,
            430,
            43,
            160,
            34,
            kControlPreset);

        tabButtons_[0] = CreateUiControl(
            L"BUTTON", L"Appearance", BS_OWNERDRAW,
            28, 96, 180, 34, kControlPageAppearance);
        tabButtons_[1] = CreateUiControl(
            L"BUTTON", L"Sampling", BS_OWNERDRAW,
            220, 96, 180, 34, kControlPageSampling);
        tabButtons_[2] = CreateUiControl(
            L"BUTTON", L"Contrast", BS_OWNERDRAW,
            412, 96, 180, 34, kControlPageContrast);

        AddSlider(0, kSliderBlurWidth, L"Horizontal color blur width", 0, 200,
                  &chameleon::Settings::horizontalBlurPixels, L" px", 0);
        AddSlider(0, kSliderBlurStrength, L"Blur strength", 0, 100,
                  &chameleon::Settings::blurStrengthPercent, L"%", 1);
        AddSlider(0, kSliderTransition, L"Color transition time", 0, 1000,
                  &chameleon::Settings::transitionMilliseconds, L" ms", 2);
        AddSlider(0, kSliderSaturation, L"Color saturation", 0, 200,
                  &chameleon::Settings::saturationPercent, L"%", 3);
        AddSlider(0, kSliderBrightness, L"Brightness", 50, 150,
                  &chameleon::Settings::brightnessPercent, L"%", 4);

        backgroundOnlyCheckbox_ = CreateUiControl(
            L"BUTTON",
            L"Background colors only — ignore text, icons, and thin dividers",
            BS_OWNERDRAW,
            40,
            140,
            520,
            28,
            kControlBackgroundOnly,
            1);
        AddSlider(1, kSliderSampleDepth, L"Sampling zone depth", 1, 200,
                  &chameleon::Settings::sampleDepth, L" px", 0, 176);
        AddSlider(1, kSliderSampleOffset, L"Sampling zone distance from taskbar", 0, 160,
                  &chameleon::Settings::sampleOffset, L" px", 1, 176);
        AddSlider(1, kSliderCellWidth, L"Horizontal segment width", 1, 24,
                  &chameleon::Settings::cellWidth, L" px", 2, 176);
        AddSlider(1, kSliderEdgeProtection, L"Split-window edge protection", 10, 180,
                  &chameleon::Settings::edgeThreshold, L"", 3, 176);
        AddSlider(1, kSliderUpdateInterval, L"Update interval", 0, 250,
                  &chameleon::Settings::intervalMilliseconds, L" ms", 4, 176);
        AddSlider(1, kSliderDetailRejection, L"Ignore foreground details up to", 0, 192,
                  &chameleon::Settings::detailRejectionPixels, L" px", 5, 176);

        adaptiveContrastCheckbox_ = CreateUiControl(
            L"BUTTON",
            L"Switch tray glyphs and running-app marks between light and dark",
            BS_OWNERDRAW,
            34,
            144,
            540,
            28,
            kControlAdaptiveContrast,
            2);
        AddSlider(2, kSliderLightThreshold, L"Use light glyphs below", 1, 90,
                  &chameleon::Settings::lightGlyphThresholdPercent, L"%", 1);
        AddSlider(2, kSliderDarkThreshold, L"Return to dark glyphs above", 2, 95,
                  &chameleon::Settings::darkGlyphThresholdPercent, L"%", 2);
        startWithWindowsCheckbox_ = CreateUiControl(
            L"BUTTON", L"Start ChameleonBar with Windows", BS_OWNERDRAW,
            34, 324, 300, 30, kControlStartWithWindows, 2);
        launchHiddenCheckbox_ = CreateUiControl(
            L"BUTTON", L"When ChameleonBar starts, show only its tray icon", BS_OWNERDRAW,
            34, 358, 500, 30, kControlLaunchHidden, 2);
        legacyTransparencyCheckbox_ = CreateUiControl(
            L"BUTTON", L"Use legacy transparency fallback", BS_OWNERDRAW,
            34, 392, 350, 30, kControlLegacyTransparency, 2);

        transparentOnDesktopCheckbox_ = CreateUiControl(
            L"BUTTON",
            L"Fully transparent when only the desktop is visible",
            BS_OWNERDRAW,
            40,
            444,
            500,
            28,
            kControlTransparentOnDesktop,
            0);
        freezeOnTaskbarActivityCheckbox_ = CreateUiControl(
            L"BUTTON",
            L"Freeze colors while taskbar previews and popups are open",
            BS_OWNERDRAW,
            40,
            478,
            520,
            28,
            kControlFreezeOnTaskbarActivity,
            0);

        statusLabel_ = CreateUiControl(
            L"STATIC",
            L"Changes apply instantly and save automatically.",
            SS_LEFT,
            28,
            604,
            360,
            24,
            0);
        CreateUiControl(
            L"BUTTON", L"Setup", BS_OWNERDRAW, 400, 596, 82, 34,
            kControlOpenSetup);
        CreateUiControl(
            L"BUTTON", L"Reset", BS_OWNERDRAW, 490, 596, 82, 34,
            kControlReset);
        CreateUiControl(
            L"BUTTON", L"Hide to tray", BS_OWNERDRAW, 430, 646, 142, 36,
            kControlHide);

        ShowSettingsPage(0);
    }

    void ShowSettingsPage(int page) {
        currentPage_ = std::clamp(page, 0, 2);
        for (int index = 0; index < static_cast<int>(pageControls_.size()); ++index) {
            for (HWND control : pageControls_[static_cast<std::size_t>(index)]) {
                ShowWindow(control, index == currentPage_ ? SW_SHOW : SW_HIDE);
            }
        }
        for (HWND tabButton : tabButtons_) {
            if (tabButton) {
                InvalidateRect(tabButton, nullptr, TRUE);
            }
        }
    }

    SliderBinding* FindSlider(HWND slider) {
        auto iterator = std::find_if(
            sliders_.begin(), sliders_.end(),
            [slider](const SliderBinding& binding) {
                return binding.slider == slider;
            });
        return iterator == sliders_.end() ? nullptr : &*iterator;
    }

    void UpdateSliderLabel(const SliderBinding& binding) {
        const int value = settings_.*(binding.member);
        std::wstring text;
        if ((binding.id == kSliderBlurWidth ||
             binding.id == kSliderTransition) &&
            value == 0) {
            text = L"Off";
        } else if (binding.id == kSliderUpdateInterval && value == 0) {
            text = L"Instant";
        } else {
            text = std::to_wstring(value) + binding.suffix;
        }
        SetWindowTextW(binding.valueLabel, text.c_str());
    }

    void PopulateSettingsControls() {
        if (!window_) {
            return;
        }
        populatingControls_ = true;
        SetWindowTextW(
            presetCombo_, chameleon::PresetName(settings_.preset).c_str());
        InvalidateRect(presetCombo_, nullptr, TRUE);
        for (const SliderBinding& binding : sliders_) {
            SendMessageW(binding.slider, TBM_SETPOS, TRUE,
                         settings_.*(binding.member));
            UpdateSliderLabel(binding);
        }
        Button_SetCheck(
            adaptiveContrastCheckbox_,
            settings_.adaptiveTrayContrast ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(
            backgroundOnlyCheckbox_,
            settings_.backgroundOnly ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(
            transparentOnDesktopCheckbox_,
            settings_.transparentOnDesktop ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(
            freezeOnTaskbarActivityCheckbox_,
            settings_.freezeOnTaskbarActivity ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(
            startWithWindowsCheckbox_,
            chameleon::IsRunAtStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(
            launchHiddenCheckbox_,
            settings_.launchHidden ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(
            legacyTransparencyCheckbox_,
            settings_.autoTransparency ? BST_CHECKED : BST_UNCHECKED);
        for (HWND checkbox : {
                 adaptiveContrastCheckbox_,
                 backgroundOnlyCheckbox_,
                 transparentOnDesktopCheckbox_,
                 freezeOnTaskbarActivityCheckbox_,
                 startWithWindowsCheckbox_,
                 launchHiddenCheckbox_,
                 legacyTransparencyCheckbox_}) {
            if (checkbox) {
                InvalidateRect(checkbox, nullptr, TRUE);
            }
        }
        populatingControls_ = false;
    }

    static VOID CALLBACK ImmediateSamplingCallback(
        PVOID parameter, BOOLEAN) {
        auto* self = static_cast<Application*>(parameter);
        if (self && self->window_ &&
            !self->immediateSamplePending_.exchange(true)) {
            if (!PostMessageW(
                    self->window_, kImmediateSamplingMessage, 0, 0)) {
                self->immediateSamplePending_ = false;
            }
        }
    }

    void StopSamplingSchedule() {
        if (window_) {
            KillTimer(window_, kSamplingTimer);
        }
        if (immediateSamplingTimer_) {
            DeleteTimerQueueTimer(
                nullptr, immediateSamplingTimer_, INVALID_HANDLE_VALUE);
            immediateSamplingTimer_ = nullptr;
        }
        immediateSamplePending_ = false;
        scheduledIntervalMilliseconds_ = -1;
    }

    void UpdateSamplingSchedule() {
        const int desiredInterval = tuning_.intervalMilliseconds;
        if (scheduledIntervalMilliseconds_ == desiredInterval &&
            (desiredInterval != 0 || immediateSamplingTimer_)) {
            return;
        }
        StopSamplingSchedule();
        if (!window_) {
            return;
        }
        if (desiredInterval == 0) {
            if (CreateTimerQueueTimer(
                &immediateSamplingTimer_,
                nullptr,
                ImmediateSamplingCallback,
                this,
                0,
                1,
                WT_EXECUTEDEFAULT)) {
                scheduledIntervalMilliseconds_ = desiredInterval;
            }
        } else {
            if (SetTimer(
                window_, kSamplingTimer,
                desiredInterval, nullptr)) {
                scheduledIntervalMilliseconds_ = desiredInterval;
            }
        }
    }

    void QueueSettingsSave() {
        if (window_) {
            SetTimer(window_, kSettingsSaveTimer, 180, nullptr);
        }
    }

    void SaveSettingsNow() {
        if (window_) {
            KillTimer(window_, kSettingsSaveTimer);
        }
        chameleon::SaveSettings(settings_);
    }

    void ApplySettingsLive() {
        tuning_ = chameleon::TuningForSettings(settings_);
        QueueSettingsSave();
        UpdateSamplingSchedule();
        for (const auto& backdrop : backdrops_) {
            SetTaskbarTransparent(
                backdrop->taskbar(), !paused_ && settings_.autoTransparency);
        }
    }

    void AttachBridge() {
        if (!activeEvent_) {
            bridgeResult_.status = HRESULT_FROM_WIN32(GetLastError());
            bridgeResult_.message = L"Local bridge event could not be created";
            UpdateBridgeStatus();
            return;
        }
        bridgeResult_ = chameleon::AttachLocalXamlBridge();
        wchar_t statusCode[16]{};
        swprintf_s(statusCode, L"0x%08lX",
                   static_cast<unsigned long>(bridgeResult_.status));
        WritePrivateProfileStringW(
            L"diagnostics", L"bridge_status", statusCode,
            chameleon::SettingsPath().c_str());
        WritePrivateProfileStringW(
            L"diagnostics", L"bridge_message", bridgeResult_.message.c_str(),
            chameleon::SettingsPath().c_str());
        UpdateBridgeStatus();
    }

    void UpdateBridgeStatus() {
        if (!statusLabel_) {
            return;
        }
        SetWindowTextW(
            statusLabel_,
            bridgeResult_.attached
                ? L"Local bridge active — contrast ready."
                : L"Bridge unavailable — open Setup.");
    }

    void DeactivateBridge() {
        if (activeEvent_) {
            ResetEvent(activeEvent_);
        }
    }

    void HandleSliderChange(HWND slider) {
        if (populatingControls_) {
            return;
        }
        SliderBinding* binding = FindSlider(slider);
        if (!binding) {
            return;
        }
        settings_.*(binding->member) =
            static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));

        if (binding->id == kSliderLightThreshold &&
            settings_.lightGlyphThresholdPercent >=
                settings_.darkGlyphThresholdPercent) {
            settings_.darkGlyphThresholdPercent =
                std::min(95, settings_.lightGlyphThresholdPercent + 1);
        } else if (binding->id == kSliderDarkThreshold &&
                   settings_.darkGlyphThresholdPercent <=
                       settings_.lightGlyphThresholdPercent) {
            settings_.lightGlyphThresholdPercent =
                std::max(1, settings_.darkGlyphThresholdPercent - 1);
        }

        UpdateSliderLabel(*binding);
        for (const SliderBinding& candidate : sliders_) {
            if (candidate.id == kSliderLightThreshold ||
                candidate.id == kSliderDarkThreshold) {
                SendMessageW(
                    candidate.slider,
                    TBM_SETPOS,
                    TRUE,
                    settings_.*(candidate.member));
                UpdateSliderLabel(candidate);
                RedrawWindow(
                    candidate.slider,
                    nullptr,
                    nullptr,
                    RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            }
        }
        settings_.preset = chameleon::SamplingPreset::Custom;
        SetWindowTextW(
            presetCombo_, chameleon::PresetName(settings_.preset).c_str());
        InvalidateRect(presetCombo_, nullptr, TRUE);
        RedrawWindow(
            slider,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        ApplySettingsLive();
    }

    void ShowSettingsWindow() {
        if (!window_) {
            return;
        }
        PopulateSettingsControls();
        ShowWindow(window_, SW_RESTORE);
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }

    void HideSettingsWindow() {
        if (window_) {
            SaveSettingsNow();
            ShowWindow(window_, SW_HIDE);
        }
    }

    void AddTrayIcon() {
        trayIcon_.cbSize = sizeof(trayIcon_);
        trayIcon_.hWnd = window_;
        trayIcon_.uID = 1;
        trayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        trayIcon_.uCallbackMessage = kTrayMessage;
        trayIcon_.hIcon = LoadApplicationIcon(
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        wcscpy_s(trayIcon_.szTip, L"ChameleonBar — sampling the screen above each taskbar");
        Shell_NotifyIconW(NIM_ADD, &trayIcon_);
        trayIcon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &trayIcon_);
        trayAdded_ = true;
    }

    void RemoveTrayIcon() {
        if (trayAdded_) {
            Shell_NotifyIconW(NIM_DELETE, &trayIcon_);
            trayAdded_ = false;
        }
    }

    void RestoreTaskbars() {
        for (const auto& backdrop : backdrops_) {
            backdrop->Hide();
            SetTaskbarTransparent(backdrop->taskbar(), false);
        }
        for (HWND taskbar : EnumerateTaskbars()) {
            SetTaskbarTransparent(taskbar, false);
        }
        TestLog(L"taskbars restored");
    }

    void DiscoverTaskbars() {
        const auto taskbars = EnumerateTaskbars();
        if (testRun_) {
            TestLog(L"discovered taskbars: " + std::to_wstring(taskbars.size()));
        }
        const std::unordered_set<HWND> live(taskbars.begin(), taskbars.end());

        backdrops_.erase(
            std::remove_if(
                backdrops_.begin(),
                backdrops_.end(),
                [&live](const std::unique_ptr<Backdrop>& backdrop) {
                    return !backdrop->valid() || live.find(backdrop->taskbar()) == live.end();
                }),
            backdrops_.end());

        for (HWND taskbar : taskbars) {
            const bool known = std::any_of(
                backdrops_.begin(),
                backdrops_.end(),
                [taskbar](const std::unique_ptr<Backdrop>& backdrop) {
                    return backdrop->taskbar() == taskbar;
                });
            if (!known) {
                backdrops_.push_back(std::make_unique<Backdrop>(
                    taskbar, &surfacePublisher_));
            }
            if (!paused_ && settings_.autoTransparency) {
                SetTaskbarTransparent(taskbar, true);
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

        if (paused_) {
            return;
        }
        RefreshVisibleApplications(now);
        int updated = 0;
        int frozen = 0;
        for (auto& backdrop : backdrops_) {
            if (settings_.transparentOnDesktop &&
                !HasVisibleApplication(backdrop->taskbar())) {
                backdrop->Hide();
                continue;
            }
            if (settings_.freezeOnTaskbarActivity &&
                HasTaskbarTransient(backdrop->taskbar())) {
                backdrop->MaintainPlacement(!bridgeResult_.attached);
                ++frozen;
                continue;
            }
            if (backdrop->Update(
                    tuning_, elapsed, !bridgeResult_.attached,
                    visibleApplicationRects_)) {
                ++updated;
            }
        }
        UpdateSamplingDiagnostic(frozen > 0);
        if (testRun_ && !sampleLogged_) {
            TestLog(L"backdrops updated: " + std::to_wstring(updated));
            sampleLogged_ = true;
        }
    }

    void RefreshVisibleApplications(ULONGLONG now) {
        if (lastWindowActivityScan_ != 0 &&
            now - lastWindowActivityScan_ < kWindowActivityScanMilliseconds) {
            return;
        }
        lastWindowActivityScan_ = now;
        auto activity = chameleon::EnumerateWindowActivity();
        visibleApplicationRects_ = std::move(activity.applicationWindows);
        transientWindowRects_ = std::move(activity.transientWindows);

        const bool desktopOnly = visibleApplicationRects_.empty();
        if (!windowActivityInitialized_ || desktopOnly != desktopOnly_) {
            desktopOnly_ = desktopOnly;
            windowActivityInitialized_ = true;
            WritePrivateProfileStringW(
                L"diagnostics",
                L"surface_state",
                desktopOnly ? L"transparent desktop" : L"adaptive app",
                chameleon::SettingsPath().c_str());
            const std::wstring count =
                std::to_wstring(visibleApplicationRects_.size());
            WritePrivateProfileStringW(
                L"diagnostics",
                L"visible_app_windows",
                count.c_str(),
                chameleon::SettingsPath().c_str());
        }
    }

    bool HasTaskbarTransient(HWND taskbar) const {
        RECT taskbarRect{};
        MONITORINFO information{};
        information.cbSize = sizeof(information);
        const HMONITOR monitor =
            MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
        if (!GetWindowRect(taskbar, &taskbarRect) || !monitor ||
            !GetMonitorInfoW(monitor, &information)) {
            return false;
        }
        return std::any_of(
            transientWindowRects_.begin(),
            transientWindowRects_.end(),
            [&](const RECT& rectangle) {
                return chameleon::IsNearTaskbar(
                    rectangle, taskbarRect, information.rcMonitor);
            });
    }

    void UpdateSamplingDiagnostic(bool frozen) {
        if (samplingFrozenInitialized_ && samplingFrozen_ == frozen) {
            return;
        }
        samplingFrozen_ = frozen;
        samplingFrozenInitialized_ = true;
        WritePrivateProfileStringW(
            L"diagnostics",
            L"sampling_state",
            frozen ? L"frozen for taskbar popup" : L"live application",
            chameleon::SettingsPath().c_str());
    }

    bool HasVisibleApplication(HWND taskbar) const {
        const HMONITOR monitor =
            MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
        MONITORINFO information{};
        information.cbSize = sizeof(information);
        if (!monitor || !GetMonitorInfoW(monitor, &information)) {
            return !visibleApplicationRects_.empty();
        }
        return std::any_of(
            visibleApplicationRects_.begin(),
            visibleApplicationRects_.end(),
            [&](const RECT& rectangle) {
                return chameleon::SubstantiallyIntersects(
                    rectangle, information.rcMonitor);
            });
    }

    void TestLog(const std::wstring& message) {
        if (!testRun_ || testLogPath_.empty()) {
            return;
        }
        const int required = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required <= 1) {
            return;
        }
        std::vector<char> utf8(static_cast<std::size_t>(required));
        WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, utf8.data(), required, nullptr, nullptr);
        HANDLE file = CreateFileW(testLogPath_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(required - 1), &written, nullptr);
        const char newline[] = "\r\n";
        WriteFile(file, newline, 2, &written, nullptr);
        CloseHandle(file);
    }

    void SetPaused(bool paused) {
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
        }
    }

    void SelectPreset(chameleon::SamplingPreset preset) {
        chameleon::ApplyPreset(settings_, preset);
        PopulateSettingsControls();
        ApplySettingsLive();
    }

    void ShowPresetMenu() {
        HMENU menu = CreatePopupMenu();
        const auto append = [&](UINT id,
                                const wchar_t* label,
                                chameleon::SamplingPreset preset) {
            AppendMenuW(
                menu,
                MF_STRING | (settings_.preset == preset ? MF_CHECKED : 0),
                id,
                label);
        };
        append(kMenuPresetExact, L"Exact edges", chameleon::SamplingPreset::Exact);
        append(kMenuPresetBalanced, L"Balanced", chameleon::SamplingPreset::Balanced);
        append(kMenuPresetCalm, L"Calm / video", chameleon::SamplingPreset::Calm);
        append(kMenuPresetCustom, L"Custom", chameleon::SamplingPreset::Custom);
        RECT selector{};
        GetWindowRect(presetCombo_, &selector);
        SetForegroundWindow(window_);
        const UINT selection = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
            selector.left,
            selector.bottom + 4,
            0,
            window_,
            nullptr);
        DestroyMenu(menu);
        switch (selection) {
        case kMenuPresetExact:
            SelectPreset(chameleon::SamplingPreset::Exact);
            break;
        case kMenuPresetBalanced:
            SelectPreset(chameleon::SamplingPreset::Balanced);
            break;
        case kMenuPresetCalm:
            SelectPreset(chameleon::SamplingPreset::Calm);
            break;
        case kMenuPresetCustom:
            SelectPreset(chameleon::SamplingPreset::Custom);
            break;
        default:
            break;
        }
    }

    void ToggleAutoTransparency() {
        settings_.autoTransparency = !settings_.autoTransparency;
        chameleon::SaveSettings(settings_);
        PopulateSettingsControls();
        for (const auto& backdrop : backdrops_) {
            SetTaskbarTransparent(backdrop->taskbar(), !paused_ && settings_.autoTransparency);
        }
    }

    void ShowTrayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuOpenWindow, L"Open ChameleonBar settings");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (paused_ ? MF_CHECKED : 0), kMenuPause, L"Pause color matching");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU presetMenu = CreatePopupMenu();
        AppendMenuW(
            presetMenu,
            MF_STRING | (settings_.preset == chameleon::SamplingPreset::Exact ? MF_CHECKED : 0),
            kMenuPresetExact,
            L"Exact edges (recommended)");
        AppendMenuW(
            presetMenu,
            MF_STRING | (settings_.preset == chameleon::SamplingPreset::Balanced ? MF_CHECKED : 0),
            kMenuPresetBalanced,
            L"Balanced");
        AppendMenuW(
            presetMenu,
            MF_STRING | (settings_.preset == chameleon::SamplingPreset::Calm ? MF_CHECKED : 0),
            kMenuPresetCalm,
            L"Calm / low motion");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(presetMenu), L"Sampling style");

        AppendMenuW(
            menu,
            MF_STRING | (settings_.autoTransparency ? MF_CHECKED : 0),
            kMenuAutoTransparency,
            L"Legacy transparency fallback");
        AppendMenuW(
            menu,
            MF_STRING | (chameleon::IsRunAtStartupEnabled() ? MF_CHECKED : 0),
            kMenuRunAtStartup,
            L"Start with Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuOpenSetup, L"Setup and troubleshooting");
        AppendMenuW(menu, MF_STRING, kMenuOpenSettings, L"Open settings folder");
        AppendMenuW(menu, MF_STRING, kMenuAbout, L"About / help");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit and restore taskbar");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void OpenSettingsFolder() {
        CreateDirectoryW(chameleon::SettingsDirectory().c_str(), nullptr);
        ShellExecuteW(
            window_,
            L"open",
            L"explorer.exe",
            chameleon::SettingsDirectory().c_str(),
            nullptr,
            SW_SHOWNORMAL);
    }

    void OpenSetupGuide() {
        const std::wstring guide =
            ApplicationDirectory() + L"\\Setup and Troubleshooting.txt";
        const DWORD attributes = GetFileAttributesW(guide.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ShellExecuteW(window_, L"open", guide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        MessageBoxW(
            window_,
            L"The setup guide isn't beside ChameleonBar.exe. "
            L"Re-extract the complete ChameleonBar preview folder and try again.",
            L"ChameleonBar setup",
            MB_OK | MB_ICONWARNING);
    }

    void ShowFirstRunHelp() {
        const wchar_t* bridgeMessage = bridgeResult_.attached
            ? L"The included local taskbar bridge is active. Windhawk is not required.\n\n"
            : L"The local taskbar bridge could not attach. Open Setup and troubleshooting for the recovery steps and diagnostic details.\n\n";
        const std::wstring message =
            L"ChameleonBar is ready to sample the pixels immediately beside each taskbar.\n\n" +
            std::wstring(bridgeMessage) +
            L"Right-click the ChameleonBar tray icon to open the setup guide, pause matching, change the color profile, or start it with Windows.";
        MessageBoxW(
            window_,
            message.c_str(),
            L"ChameleonBar is running",
            MB_OK | MB_ICONINFORMATION);
    }

    void ShowAbout() {
        const std::wstring message =
            L"ChameleonBar local preview 0.9.1\n\n"
            L"Current sampling style: " + chameleon::PresetName(settings_.preset) +
            L"\n\nEach small taskbar segment follows the screen area directly beside it. "
            L"Split-screen boundaries remain sharp; text, cursors, and local video motion are stabilized. "
            L"The included local bridge makes the Windows background transparent and switches tray glyphs, clock text, and running-app marks between light and dark using the final visible pixels underneath them.\n\nBridge status: " +
            bridgeResult_.message + L"\n\n"
            L"Pause or Exit always restores the normal taskbar effect.";
        MessageBoxW(window_, message.c_str(), L"About ChameleonBar", MB_OK | MB_ICONINFORMATION);
    }

    bool IsPageControl(HWND control) const {
        for (const auto& page : pageControls_) {
            if (std::find(page.begin(), page.end(), control) != page.end()) {
                return true;
            }
        }
        return false;
    }

    LRESULT HandleControlColor(HDC dc, HWND control) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kUiText);
        return reinterpret_cast<LRESULT>(
            IsPageControl(control) ? surfaceBrush_ : backgroundBrush_);
    }

    void DrawOwnerButton(const DRAWITEMSTRUCT& item) {
        const bool primary = item.CtlID == kControlHide;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const COLORREF fill = primary
            ? (pressed ? kUiAccentPressed : kUiAccent)
            : (pressed ? RGB(47, 52, 64) : kUiSurfaceRaised);
        DrawRoundedFill(item.hDC, item.rcItem, 12, fill);

        wchar_t text[128]{};
        GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(
            item.hDC,
            disabled ? kUiMutedText : (primary ? RGB(8, 24, 20) : kUiText));
        SelectObject(item.hDC, uiFont_);
        RECT textRect = item.rcItem;
        DrawTextW(
            item.hDC,
            text,
            -1,
            &textRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void DrawOwnerCombo(const DRAWITEMSTRUCT& item) {
        RECT rect = item.rcItem;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        DrawRoundedFill(
            item.hDC,
            rect,
            10,
            pressed ? RGB(47, 52, 64) : kUiSurfaceRaised);
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(77, 86, 103));
        const HGDIOBJ oldBorderPen = SelectObject(item.hDC, borderPen);
        const HGDIOBJ oldBorderBrush = SelectObject(
            item.hDC, GetStockObject(HOLLOW_BRUSH));
        RoundRect(
            item.hDC,
            rect.left,
            rect.top,
            rect.right,
            rect.bottom,
            10,
            10);
        SelectObject(item.hDC, oldBorderBrush);
        SelectObject(item.hDC, oldBorderPen);
        DeleteObject(borderPen);
        wchar_t label[128]{};
        GetWindowTextW(item.hwndItem, label, ARRAYSIZE(label));
        RECT textRect = item.rcItem;
        textRect.left += 13;
        textRect.right -= 34;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, kUiText);
        SelectObject(item.hDC, uiFont_);
        DrawTextW(
            item.hDC, label, -1, &textRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const int centerX = rect.right - 18;
        const int centerY = (rect.top + rect.bottom) / 2 + 1;
        HPEN pen = CreatePen(PS_SOLID, 2, kUiMutedText);
        const HGDIOBJ oldPen = SelectObject(item.hDC, pen);
        MoveToEx(item.hDC, centerX - 4, centerY - 2, nullptr);
        LineTo(item.hDC, centerX, centerY + 2);
        LineTo(item.hDC, centerX + 4, centerY - 2);
        SelectObject(item.hDC, oldPen);
        DeleteObject(pen);
    }

    bool IsCheckboxId(UINT id) const {
        return id == kControlAdaptiveContrast ||
               id == kControlBackgroundOnly ||
               id == kControlTransparentOnDesktop ||
               id == kControlFreezeOnTaskbarActivity ||
               id == kControlStartWithWindows ||
               id == kControlLaunchHidden ||
               id == kControlLegacyTransparency;
    }

    bool IsCheckboxChecked(HWND checkbox) const {
        if (checkbox == adaptiveContrastCheckbox_) {
            return settings_.adaptiveTrayContrast;
        }
        if (checkbox == backgroundOnlyCheckbox_) {
            return settings_.backgroundOnly;
        }
        if (checkbox == transparentOnDesktopCheckbox_) {
            return settings_.transparentOnDesktop;
        }
        if (checkbox == freezeOnTaskbarActivityCheckbox_) {
            return settings_.freezeOnTaskbarActivity;
        }
        if (checkbox == startWithWindowsCheckbox_) {
            return chameleon::IsRunAtStartupEnabled();
        }
        if (checkbox == launchHiddenCheckbox_) {
            return settings_.launchHidden;
        }
        if (checkbox == legacyTransparencyCheckbox_) {
            return settings_.autoTransparency;
        }
        return false;
    }

    bool ToggleCheckbox(HWND checkbox) {
        const bool checked = IsCheckboxChecked(checkbox);
        InvalidateRect(checkbox, nullptr, TRUE);
        return !checked;
    }

    void DrawOwnerCheckbox(const DRAWITEMSTRUCT& item) {
        RECT rect = item.rcItem;
        FillRect(item.hDC, &rect, surfaceBrush_);
        const bool checked = IsCheckboxChecked(item.hwndItem);
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        RECT box{
            rect.left + 2,
            rect.top + (rect.bottom - rect.top - 20) / 2,
            rect.left + 22,
            rect.top + (rect.bottom - rect.top - 20) / 2 + 20,
        };
        DrawRoundedFill(
            item.hDC,
            box,
            7,
            checked
                ? (pressed ? kUiAccentPressed : kUiAccent)
                : (pressed ? RGB(48, 54, 66) : kUiSurfaceRaised));
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(8, 34, 28));
            const HGDIOBJ oldPen = SelectObject(item.hDC, pen);
            MoveToEx(item.hDC, box.left + 5, box.top + 10, nullptr);
            LineTo(item.hDC, box.left + 9, box.top + 14);
            LineTo(item.hDC, box.left + 16, box.top + 6);
            SelectObject(item.hDC, oldPen);
            DeleteObject(pen);
        } else {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(83, 91, 108));
            const HGDIOBJ oldPen = SelectObject(item.hDC, pen);
            const HGDIOBJ oldBrush = SelectObject(
                item.hDC, GetStockObject(HOLLOW_BRUSH));
            RoundRect(
                item.hDC,
                box.left,
                box.top,
                box.right,
                box.bottom,
                7,
                7);
            SelectObject(item.hDC, oldBrush);
            SelectObject(item.hDC, oldPen);
            DeleteObject(pen);
        }

        wchar_t text[256]{};
        GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
        RECT textRect{box.right + 10, rect.top, rect.right, rect.bottom};
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, kUiText);
        SelectObject(item.hDC, uiFont_);
        DrawTextW(
            item.hDC,
            text,
            -1,
            &textRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);
    }

    void DrawOwnerTab(const DRAWITEMSTRUCT& item) {
        int page = -1;
        if (item.CtlID == kControlPageAppearance) page = 0;
        if (item.CtlID == kControlPageSampling) page = 1;
        if (item.CtlID == kControlPageContrast) page = 2;
        if (page < 0) return;
        RECT rect = item.rcItem;
        const bool selected = page == currentPage_;
        DrawRoundedFill(
            item.hDC,
            rect,
            12,
            selected ? kUiSurfaceRaised : kUiBackground);

        wchar_t label[64]{};
        GetWindowTextW(item.hwndItem, label, ARRAYSIZE(label));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, selected ? kUiText : kUiMutedText);
        SelectObject(item.hDC, uiFont_);
        DrawTextW(
            item.hDC,
            label,
            -1,
            &rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (selected) {
            RECT accent{rect.left + 22, rect.bottom - 3,
                        rect.right - 22, rect.bottom};
            DrawRoundedFill(item.hDC, accent, 3, kUiAccent);
        }
    }

    LRESULT DrawModernTrackbar(NMCUSTOMDRAW& customDraw) {
        if (customDraw.dwDrawStage != CDDS_PREPAINT) {
            return CDRF_DODEFAULT;
        }
        HWND slider = customDraw.hdr.hwndFrom;
        RECT client{};
        GetClientRect(slider, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        HDC buffer = CreateCompatibleDC(customDraw.hdc);
        HBITMAP bitmap = CreateCompatibleBitmap(customDraw.hdc, width, height);
        const HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        FillRect(buffer, &client, surfaceBrush_);

        const int centerY = (client.top + client.bottom) / 2;
        const int left = client.left + 9;
        const int right = client.right - 9;
        RECT channel{left, centerY - 3, right, centerY + 3};
        DrawRoundedFill(buffer, channel, 6, RGB(62, 68, 82));

        const int minimum = static_cast<int>(
            SendMessageW(slider, TBM_GETRANGEMIN, 0, 0));
        const int maximum = static_cast<int>(
            SendMessageW(slider, TBM_GETRANGEMAX, 0, 0));
        const int position = static_cast<int>(
            SendMessageW(slider, TBM_GETPOS, 0, 0));
        const double amount = maximum > minimum
            ? static_cast<double>(position - minimum) /
                  static_cast<double>(maximum - minimum)
            : 0.0;
        const int thumbX = left + static_cast<int>(
            std::lround((right - left) * std::clamp(amount, 0.0, 1.0)));
        RECT active{left, centerY - 3, std::max(left + 1, thumbX), centerY + 3};
        DrawRoundedFill(buffer, active, 6, kUiAccent);
        RECT thumb{thumbX - 7, centerY - 7, thumbX + 7, centerY + 7};
        DrawRoundedFill(buffer, thumb, 14, kUiText);
        RECT center{thumbX - 4, centerY - 4, thumbX + 4, centerY + 4};
        DrawRoundedFill(buffer, center, 8, kUiAccent);
        BitBlt(customDraw.hdc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        return CDRF_SKIPDEFAULT;
    }

    void HandleCommand(UINT command, UINT notification = 0) {
        switch (command) {
        case kMenuOpenWindow:
            ShowSettingsWindow();
            break;
        case kMenuPause:
            SetPaused(!paused_);
            break;
        case kMenuPresetExact:
            SelectPreset(chameleon::SamplingPreset::Exact);
            break;
        case kMenuPresetBalanced:
            SelectPreset(chameleon::SamplingPreset::Balanced);
            break;
        case kMenuPresetCalm:
            SelectPreset(chameleon::SamplingPreset::Calm);
            break;
        case kMenuAutoTransparency:
            ToggleAutoTransparency();
            break;
        case kMenuRunAtStartup:
            chameleon::SetRunAtStartupEnabled(!chameleon::IsRunAtStartupEnabled());
            break;
        case kMenuOpenSettings:
            OpenSettingsFolder();
            break;
        case kMenuOpenSetup:
            OpenSetupGuide();
            break;
        case kMenuAbout:
            ShowAbout();
            break;
        case kMenuExit:
            DestroyWindow(window_);
            break;
        case kControlPageAppearance:
            ShowSettingsPage(0);
            break;
        case kControlPageSampling:
            ShowSettingsPage(1);
            break;
        case kControlPageContrast:
            ShowSettingsPage(2);
            break;
        case kControlPreset:
            if (notification == BN_CLICKED && !populatingControls_) {
                ShowPresetMenu();
            }
            break;
        case kControlHide:
            HideSettingsWindow();
            break;
        case kControlReset: {
            const bool keepFirstRunDismissed = !settings_.showFirstRunHelp;
            settings_ = chameleon::Settings{};
            settings_.showFirstRunHelp = !keepFirstRunDismissed;
            chameleon::ApplyPreset(settings_, chameleon::SamplingPreset::Exact);
            PopulateSettingsControls();
            ApplySettingsLive();
            break;
        }
        case kControlOpenSetup:
            OpenSetupGuide();
            break;
        case kControlAdaptiveContrast:
            if (notification == BN_CLICKED && !populatingControls_) {
                settings_.adaptiveTrayContrast = ToggleCheckbox(
                    adaptiveContrastCheckbox_);
                ApplySettingsLive();
            }
            break;
        case kControlBackgroundOnly:
            if (notification == BN_CLICKED && !populatingControls_) {
                settings_.backgroundOnly = ToggleCheckbox(
                    backgroundOnlyCheckbox_);
                settings_.preset = chameleon::SamplingPreset::Custom;
                ApplySettingsLive();
                PopulateSettingsControls();
            }
            break;
        case kControlTransparentOnDesktop:
            if (notification == BN_CLICKED && !populatingControls_) {
                settings_.transparentOnDesktop = ToggleCheckbox(
                    transparentOnDesktopCheckbox_);
                ApplySettingsLive();
            }
            break;
        case kControlFreezeOnTaskbarActivity:
            if (notification == BN_CLICKED && !populatingControls_) {
                settings_.freezeOnTaskbarActivity = ToggleCheckbox(
                    freezeOnTaskbarActivityCheckbox_);
                ApplySettingsLive();
            }
            break;
        case kControlStartWithWindows:
            if (notification == BN_CLICKED && !populatingControls_) {
                chameleon::SetRunAtStartupEnabled(ToggleCheckbox(
                    startWithWindowsCheckbox_));
                PopulateSettingsControls();
            }
            break;
        case kControlLaunchHidden:
            if (notification == BN_CLICKED && !populatingControls_) {
                settings_.launchHidden = ToggleCheckbox(
                    launchHiddenCheckbox_);
                ApplySettingsLive();
            }
            break;
        case kControlLegacyTransparency:
            if (notification == BN_CLICKED && !populatingControls_) {
                settings_.autoTransparency = ToggleCheckbox(
                    legacyTransparencyCheckbox_);
                ApplySettingsLive();
            }
            break;
        }
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Application* self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Application*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) {
            return DefWindowProcW(window, message, wParam, lParam);
        }

        if (message == self->taskbarCreatedMessage_) {
            self->RemoveTrayIcon();
            self->AddTrayIcon();
            self->AttachBridge();
            self->DiscoverTaskbars();
            return 0;
        }

        switch (message) {
        case kShowSettingsMessage:
            self->ShowSettingsWindow();
            return 0;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client,
                     self->backgroundBrush_);
            const RECT card{24, 132, 596, 588};
            DrawRoundedFill(
                reinterpret_cast<HDC>(wParam), card, 18, kUiSurface);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return self->HandleControlColor(
                reinterpret_cast<HDC>(wParam),
                reinterpret_cast<HWND>(lParam));
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!item) {
                return FALSE;
            }
            if (item->CtlID == kControlPageAppearance ||
                item->CtlID == kControlPageSampling ||
                item->CtlID == kControlPageContrast) {
                self->DrawOwnerTab(*item);
                return TRUE;
            }
            if (item->CtlID == kControlPreset) {
                self->DrawOwnerCombo(*item);
                return TRUE;
            }
            if (self->IsCheckboxId(item->CtlID)) {
                self->DrawOwnerCheckbox(*item);
                return TRUE;
            }
            if (item->CtlID == kControlOpenSetup ||
                item->CtlID == kControlReset ||
                item->CtlID == kControlHide) {
                self->DrawOwnerButton(*item);
                return TRUE;
            }
            return FALSE;
        }
        case WM_CLOSE:
            self->HideSettingsWindow();
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                self->HideSettingsWindow();
            }
            return 0;
        case WM_HSCROLL:
            self->HandleSliderChange(reinterpret_cast<HWND>(lParam));
            return 0;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header && header->code == NM_CUSTOMDRAW) {
                wchar_t className[64]{};
                GetClassNameW(
                    header->hwndFrom, className, ARRAYSIZE(className));
                if (wcscmp(className, TRACKBAR_CLASSW) == 0) {
                    return self->DrawModernTrackbar(
                        *reinterpret_cast<NMCUSTOMDRAW*>(lParam));
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == kSamplingTimer) {
                self->Sample();
            } else if (wParam == kTestExitTimer) {
                DestroyWindow(window);
            } else if (wParam == kSettingsSaveTimer) {
                self->SaveSettingsNow();
            }
            return 0;
        case kImmediateSamplingMessage:
            if (self->tuning_.intervalMilliseconds == 0) {
                self->Sample();
            }
            self->immediateSamplePending_ = false;
            return 0;
        case WM_COMMAND:
            self->HandleCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case kTrayMessage:
            if (LOWORD(lParam) == WM_CONTEXTMENU ||
                LOWORD(lParam) == WM_RBUTTONUP) {
                self->ShowTrayMenu();
            } else if (LOWORD(lParam) == NIN_SELECT ||
                       LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                self->ShowSettingsWindow();
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            self->DiscoverTaskbars();
            return 0;
        case WM_DESTROY:
            self->StopSamplingSchedule();
            self->SaveSettingsNow();
            self->DeactivateBridge();
            self->RestoreTaskbars();
            self->RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    friend bool RegisterWindowClasses();

    HWND window_ = nullptr;
    std::array<HWND, 3> tabButtons_{};
    HWND presetCombo_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND adaptiveContrastCheckbox_ = nullptr;
    HWND backgroundOnlyCheckbox_ = nullptr;
    HWND transparentOnDesktopCheckbox_ = nullptr;
    HWND freezeOnTaskbarActivityCheckbox_ = nullptr;
    HWND startWithWindowsCheckbox_ = nullptr;
    HWND launchHiddenCheckbox_ = nullptr;
    HWND legacyTransparencyCheckbox_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH surfaceBrush_ = nullptr;
    std::array<std::vector<HWND>, 3> pageControls_;
    std::vector<SliderBinding> sliders_;
    int currentPage_ = 0;
    bool populatingControls_ = false;
    UINT taskbarCreatedMessage_ = 0;
    NOTIFYICONDATAW trayIcon_{};
    bool trayAdded_ = false;
    bool paused_ = false;
    chameleon::Settings settings_{};
    chameleon::RuntimeTuning tuning_ =
        chameleon::TuningForSettings(chameleon::Settings{});
    chameleon::SurfacePublisher surfacePublisher_;
    std::vector<std::unique_ptr<Backdrop>> backdrops_;
    ULONGLONG lastTick_ = 0;
    ULONGLONG lastDiscovery_ = 0;
    ULONGLONG lastWindowActivityScan_ = 0;
    std::vector<RECT> visibleApplicationRects_;
    std::vector<RECT> transientWindowRects_;
    bool windowActivityInitialized_ = false;
    bool desktopOnly_ = false;
    bool samplingFrozenInitialized_ = false;
    bool samplingFrozen_ = false;
    bool testRun_ = false;
    bool sampleLogged_ = false;
    HANDLE immediateSamplingTimer_ = nullptr;
    std::atomic_bool immediateSamplePending_{false};
    int scheduledIntervalMilliseconds_ = -1;
    std::wstring testLogPath_;
    HANDLE activeEvent_ = nullptr;
    chameleon::BridgeAttachResult bridgeResult_{};
};

bool RegisterWindowClasses() {
    WNDCLASSEXW controller{};
    controller.cbSize = sizeof(controller);
    controller.hInstance = g_instance;
    controller.lpfnWndProc = Application::WindowProcedure;
    controller.lpszClassName = kControllerClass;
    controller.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    controller.hIcon = LoadApplicationIcon(
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    controller.hIconSm = LoadApplicationIcon(
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    controller.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&controller) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW backdrop{};
    backdrop.cbSize = sizeof(backdrop);
    backdrop.hInstance = g_instance;
    backdrop.lpfnWndProc = Backdrop::WindowProcedure;
    backdrop.lpszClassName = kBackdropClass;
    backdrop.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    backdrop.hbrBackground = nullptr;
    if (!RegisterClassExW(&backdrop) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    INITCOMMONCONTROLSEX commonControls{
        sizeof(commonControls), ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&commonControls);
    using SetDpiAwarenessFunction = BOOL(WINAPI*)(HANDLE);
    const auto setDpiAwareness = reinterpret_cast<SetDpiAwarenessFunction>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setDpiAwareness) {
        setDpiAwareness(reinterpret_cast<HANDLE>(-4));  // Per-monitor aware v2.
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ChameleonBar.SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kControllerClass, nullptr)) {
            PostMessageW(existing, kShowSettingsMessage, 0, 0);
        } else {
            MessageBoxW(
                nullptr,
                L"ChameleonBar is already running in the notification area.",
                kApplicationName,
                MB_OK);
        }
        if (mutex) {
            CloseHandle(mutex);
        }
        return 0;
    }

    if (!RegisterWindowClasses()) {
        MessageBoxW(nullptr, L"ChameleonBar could not register its Windows components.", kApplicationName, MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    const bool testRun = wcsstr(GetCommandLineW(), L"--test-run") != nullptr;
    Application application(testRun);
    if (!application.Initialize()) {
        MessageBoxW(nullptr, L"ChameleonBar could not start.", kApplicationName, MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    const int result = application.Run();
    CloseHandle(mutex);
    return result;
}
