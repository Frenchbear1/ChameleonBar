// ChameleonBar local Windows 11 XAML bridge.
//
// This file is distributed under GNU GPL v3.0. Its small XAML diagnostics
// attachment layer is derived from Windows 11 Taskbar Styler by m417z:
// https://github.com/ramensoftware/windhawk-mods/blob/main/mods/windows-11-taskbar-styler.wh.cpp
//
// Only the behavior ChameleonBar needs is retained: make the native taskbar
// background and stroke transparent, and keep the native system-tray foreground
// and running-app indicators readable. No generic selector/style engine, themes,
// networking, analytics, or Windhawk API dependency is included.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>

#include "../src/surface_protocol.h"

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace controls = winrt::Windows::UI::Xaml::Controls;
namespace media = winrt::Windows::UI::Xaml::Media;
namespace imaging = winrt::Windows::UI::Xaml::Media::Imaging;
namespace shapes = winrt::Windows::UI::Xaml::Shapes;

struct IBufferByteAccess : ::IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Buffer(std::uint8_t** value) = 0;
};

constexpr GUID kIidBufferByteAccess = {
    0x905a0fef,
    0xbc53,
    0x11df,
    {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

namespace {

constexpr wchar_t kActiveEventName[] = L"Local\\ChameleonBar.Active.v4";
constexpr wchar_t kBridgeReadyEventName[] =
    L"Local\\ChameleonBar.XamlBridgeReady.v4";

// {7B5E11EE-9EC1-4BA2-AD74-2872EAF766FB}
constexpr CLSID kChameleonBarTap = {
    0x7b5e11ee,
    0x9ec1,
    0x4ba2,
    {0xad, 0x74, 0x28, 0x72, 0xea, 0xf7, 0x66, 0xfb}};

UINT WindowDpi(HWND window) {
    using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow =
        reinterpret_cast<GetDpiForWindowFunction>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return getDpiForWindow ? getDpiForWindow(window) : 96;
}

HANDLE g_readyEvent;

void DebugLog(const wchar_t* text) {
    OutputDebugStringW(L"ChameleonBar.XamlBridge: ");
    OutputDebugStringW(text);
    OutputDebugStringW(L"\n");
}

bool IsChameleonBarActive() {
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kActiveEventName);
    if (!event) {
        return false;
    }
    const bool active = WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    CloseHandle(event);
    return active;
}

struct ContrastSettings {
    bool enabled = true;
    double lightThreshold = 0.18;
    double darkThreshold = 0.26;
};

std::wstring LocalSettingsPath() {
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
    if (!length || length >= ARRAYSIZE(localAppData)) {
        return {};
    }
    std::wstring path(localAppData, length);
    path += L"\\ChameleonBar\\settings.ini";
    return path;
}

ContrastSettings ReadContrastSettings() {
    ContrastSettings settings;
    const std::wstring path = LocalSettingsPath();
    if (path.empty()) {
        return settings;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return settings;
    }

    settings.enabled =
        GetPrivateProfileIntW(L"contrast", L"enabled", 1, path.c_str()) != 0;
    settings.lightThreshold =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(
                       L"contrast", L"light_threshold", 18, path.c_str())),
                   1, 90) /
        100.0;
    settings.darkThreshold =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(
                       L"contrast", L"dark_threshold", 26, path.c_str())),
                   2, 95) /
        100.0;
    if (settings.darkThreshold <= settings.lightThreshold) {
        settings.darkThreshold = std::min(0.95, settings.lightThreshold + 0.05);
    }
    return settings;
}

struct TrayContext;
void WriteTrayDiagnostic(const TrayContext& context);

wux::FrameworkElement ParentElement(wux::FrameworkElement element) {
    return media::VisualTreeHelper::GetParent(element)
        .try_as<wux::FrameworkElement>();
}

bool HasAncestorClass(wux::FrameworkElement element,
                      const wchar_t* className,
                      const wchar_t* excludedName = nullptr) {
    for (auto parent = ParentElement(element); parent;
         parent = ParentElement(parent)) {
        if (excludedName && parent.Name() == excludedName) {
            return false;
        }
        if (winrt::get_class_name(parent) == className) {
            return true;
        }
    }
    return false;
}

void VisitDescendants(
    wux::FrameworkElement element,
    const std::function<void(wux::FrameworkElement)>& callback) {
    const int count = media::VisualTreeHelper::GetChildrenCount(element);
    for (int index = 0; index < count; ++index) {
        auto child = media::VisualTreeHelper::GetChild(element, index)
                         .try_as<wux::FrameworkElement>();
        if (!child) {
            continue;
        }
        callback(child);
        VisitDescendants(child, callback);
    }
}

wux::FrameworkElement FindDescendantByName(wux::FrameworkElement element,
                                           const wchar_t* name) {
    wux::FrameworkElement found = nullptr;
    VisitDescendants(element, [&](wux::FrameworkElement child) {
        if (!found && child.Name() == name) {
            found = child;
        }
    });
    return found;
}

struct BackgroundOverride {
    winrt::weak_ref<shapes::Shape> element;
    media::Brush original{nullptr};
    HWND taskbar = nullptr;
    bool overridden = false;
};

struct SurfaceSnapshot {
    bool visible = false;
    int pixelWidth = 0;
    int pixelHeight = 0;
    LONG revision = 0;
    std::vector<std::uint32_t> pixels;
};

struct SurfaceBrushState {
    HWND taskbar = nullptr;
    int pixelWidth = 0;
    int pixelHeight = 0;
    LONG revision = -1;
    imaging::WriteableBitmap bitmap{nullptr};
    media::ImageBrush brush{nullptr};
};

struct TextForegroundOverride {
    winrt::weak_ref<controls::TextBlock> element;
    media::Brush original{nullptr};
};

struct IconForegroundOverride {
    winrt::weak_ref<controls::IconElement> element;
    media::Brush original{nullptr};
};

struct TrayContext {
    winrt::weak_ref<wux::FrameworkElement> frame;
    winrt::weak_ref<wux::FrameworkElement> themeTarget;
    HWND taskbar = nullptr;
    wux::ElementTheme originalFrameTheme = wux::ElementTheme::Default;
    wux::ElementTheme originalTargetTheme = wux::ElementTheme::Default;
    bool initialized = false;
    bool lightGlyphs = false;
    bool overridesApplied = false;
    double filteredLuminance = 0.0;
    std::vector<TextForegroundOverride> textTargets;
    std::vector<IconForegroundOverride> iconTargets;
};

struct RunningIndicatorOverride {
    winrt::weak_ref<shapes::Shape> element;
    media::Brush original{nullptr};
    media::SolidColorBrush applied{nullptr};
    HWND taskbar = nullptr;
    bool initialized = false;
    bool light = false;
    bool overridden = false;
    double filteredLuminance = 0.0;
};

void WriteTrayDiagnostic(const TrayContext& context) {
    const std::wstring path = LocalSettingsPath();
    if (path.empty()) {
        return;
    }
    const std::wstring luminance = std::to_wstring(static_cast<int>(
        std::lround(context.filteredLuminance * 100.0)));
    const std::wstring textTargets =
        std::to_wstring(context.textTargets.size());
    const std::wstring iconTargets =
        std::to_wstring(context.iconTargets.size());
    WritePrivateProfileStringW(
        L"diagnostics", L"tray_mode",
        context.lightGlyphs ? L"light" : L"dark", path.c_str());
    WritePrivateProfileStringW(
        L"diagnostics", L"tray_luminance_percent", luminance.c_str(),
        path.c_str());
    WritePrivateProfileStringW(
        L"diagnostics", L"tray_text_targets", textTargets.c_str(),
        path.c_str());
    WritePrivateProfileStringW(
        L"diagnostics", L"tray_icon_targets", iconTargets.c_str(),
        path.c_str());
}

thread_local std::vector<BackgroundOverride> g_backgrounds;
thread_local std::vector<SurfaceBrushState> g_surfaceBrushes;
thread_local std::vector<std::shared_ptr<TrayContext>> g_trays;
thread_local std::vector<RunningIndicatorOverride> g_runningIndicators;
thread_local media::SolidColorBrush g_transparentBrush{nullptr};
thread_local unsigned int g_backgroundGuardCorrections;
thread_local wux::DispatcherTimer g_pulseTimer{nullptr};
thread_local winrt::event_token g_pulseToken{};
thread_local ULONGLONG g_lastSettingsRead;
thread_local ContrastSettings g_contrastSettings;
thread_local bool g_applyingBackground;
thread_local bool g_surfaceDiagnosticWritten;
HANDLE g_surfaceMapping;
const chameleon::SharedSurfaceState* g_surfaceState;

bool EnsureSurfaceMapping() {
    if (g_surfaceState) {
        return true;
    }
    g_surfaceMapping = OpenFileMappingW(
        FILE_MAP_READ, FALSE, chameleon::kSurfaceMappingName);
    if (!g_surfaceMapping) {
        return false;
    }
    g_surfaceState = static_cast<const chameleon::SharedSurfaceState*>(
        MapViewOfFile(
            g_surfaceMapping,
            FILE_MAP_READ,
            0,
            0,
            sizeof(chameleon::SharedSurfaceState)));
    if (!g_surfaceState) {
        CloseHandle(g_surfaceMapping);
        g_surfaceMapping = nullptr;
        return false;
    }
    return true;
}

std::optional<SurfaceSnapshot> ReadSurface(HWND taskbar) {
    if (!EnsureSurfaceMapping() ||
        g_surfaceState->version != chameleon::kSurfaceProtocolVersion) {
        return std::nullopt;
    }
    const auto expected = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(taskbar));
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = g_surfaceState->sequence;
        MemoryBarrier();
        if ((before & 1) != 0) {
            continue;
        }
        for (const auto& slot : g_surfaceState->slots) {
            if (slot.taskbarWindow != expected) {
                continue;
            }
            const int count = std::clamp(
                static_cast<int>(slot.pixelCount),
                0,
                static_cast<int>(chameleon::kMaximumSurfacePixels));
            SurfaceSnapshot snapshot;
            snapshot.visible = slot.visible != 0;
            snapshot.pixelWidth = std::max(0, static_cast<int>(slot.pixelWidth));
            snapshot.pixelHeight = std::max(0, static_cast<int>(slot.pixelHeight));
            snapshot.revision = slot.revision;
            if (snapshot.visible && count > 0 &&
                snapshot.pixelWidth * snapshot.pixelHeight == count) {
                snapshot.pixels.assign(slot.pixels, slot.pixels + count);
            } else if (snapshot.visible) {
                snapshot.visible = false;
            }
            MemoryBarrier();
            const LONG after = g_surfaceState->sequence;
            if (before == after && (after & 1) == 0) {
                return snapshot;
            }
            break;
        }
        MemoryBarrier();
        if (before == g_surfaceState->sequence) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

SurfaceBrushState& SurfaceBrushFor(HWND taskbar) {
    const auto iterator = std::find_if(
        g_surfaceBrushes.begin(),
        g_surfaceBrushes.end(),
        [taskbar](const SurfaceBrushState& state) {
            return state.taskbar == taskbar;
        });
    if (iterator != g_surfaceBrushes.end()) {
        return *iterator;
    }
    g_surfaceBrushes.push_back({});
    g_surfaceBrushes.back().taskbar = taskbar;
    return g_surfaceBrushes.back();
}

media::Brush BrushForSurface(HWND taskbar, const SurfaceSnapshot& snapshot) {
    if (!snapshot.visible || snapshot.pixels.empty()) {
        return g_transparentBrush;
    }
    auto& state = SurfaceBrushFor(taskbar);
    if (!state.bitmap || state.pixelWidth != snapshot.pixelWidth ||
        state.pixelHeight != snapshot.pixelHeight) {
        state.pixelWidth = snapshot.pixelWidth;
        state.pixelHeight = snapshot.pixelHeight;
        state.revision = -1;
        state.bitmap = imaging::WriteableBitmap(
            snapshot.pixelWidth, snapshot.pixelHeight);
        state.brush = media::ImageBrush();
        state.brush.ImageSource(state.bitmap);
        state.brush.Stretch(media::Stretch::Fill);
    }
    if (state.revision != snapshot.revision) {
        const auto pixelBuffer = state.bitmap.PixelBuffer();
        winrt::com_ptr<IBufferByteAccess> byteAccess;
        winrt::check_hresult(
            reinterpret_cast<IUnknown*>(winrt::get_abi(pixelBuffer))
                ->QueryInterface(
                    kIidBufferByteAccess, byteAccess.put_void()));
        std::uint8_t* destination = nullptr;
        winrt::check_hresult(byteAccess->Buffer(&destination));
        std::memcpy(
            destination,
            snapshot.pixels.data(),
            snapshot.pixels.size() * sizeof(std::uint32_t));
        state.bitmap.Invalidate();
        state.revision = snapshot.revision;
    }
    return state.brush;
}

void RecordBackgroundGuardCorrection() {
    ++g_backgroundGuardCorrections;
    const std::wstring path = LocalSettingsPath();
    if (path.empty()) {
        return;
    }
    const std::wstring count =
        std::to_wstring(g_backgroundGuardCorrections);
    WritePrivateProfileStringW(
        L"diagnostics", L"background_guard_corrections", count.c_str(),
        path.c_str());
}

bool IsTaskbarWindow(HWND window) {
    wchar_t className[64]{};
    GetClassNameW(window, className, ARRAYSIZE(className));
    return wcscmp(className, L"Shell_TrayWnd") == 0 ||
           wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

HWND ResolveTaskbarWindow(wux::FrameworkElement frame) {
    const auto root = frame.XamlRoot();
    if (!root) {
        return nullptr;
    }
    const auto rootSize = root.Size();
    const double scale = root.RasterizationScale();
    const int expectedWidth = static_cast<int>(std::lround(rootSize.Width * scale));
    const int expectedHeight = static_cast<int>(std::lround(rootSize.Height * scale));

    struct Search {
        int expectedWidth;
        int expectedHeight;
        HWND best = nullptr;
        int bestScore = INT_MAX;
    } search{expectedWidth, expectedHeight};

    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto* search = reinterpret_cast<Search*>(parameter);
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId != GetCurrentProcessId() || !IsTaskbarWindow(window)) {
                return TRUE;
            }
            RECT rect{};
            if (!GetWindowRect(window, &rect)) {
                return TRUE;
            }
            const int width = rect.right - rect.left;
            const int height = rect.bottom - rect.top;
            const int score = std::abs(width - search->expectedWidth) +
                              std::abs(height - search->expectedHeight);
            if (score < search->bestScore) {
                search->bestScore = score;
                search->best = window;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}

double LinearChannel(std::uint8_t channel) {
    const double srgb = static_cast<double>(channel) / 255.0;
    return srgb <= 0.04045 ? srgb / 12.92
                           : std::pow((srgb + 0.055) / 1.055, 2.4);
}

double PixelLuminance(std::uint32_t bgra) {
    const auto blue = static_cast<std::uint8_t>(bgra & 0xFF);
    const auto green = static_cast<std::uint8_t>((bgra >> 8) & 0xFF);
    const auto red = static_cast<std::uint8_t>((bgra >> 16) & 0xFF);
    return 0.2126 * LinearChannel(red) + 0.7152 * LinearChannel(green) +
           0.0722 * LinearChannel(blue);
}

std::optional<double> SampleTrayLuminance(TrayContext& context,
                                          wux::FrameworkElement frame) {
    if (!context.taskbar || !IsWindow(context.taskbar)) {
        context.taskbar = ResolveTaskbarWindow(frame);
    }
    if (!context.taskbar) {
        return std::nullopt;
    }

    RECT taskbarRect{};
    if (!GetWindowRect(context.taskbar, &taskbarRect)) {
        return std::nullopt;
    }
    const UINT dpi = WindowDpi(context.taskbar);
    const double scale = static_cast<double>(dpi ? dpi : 96) / 96.0;
    const auto topLeft = frame.TransformToVisual(nullptr).TransformPoint({0, 0});
    const LONG frameLeft = taskbarRect.left +
                           static_cast<LONG>(std::lround(topLeft.X * scale));
    const LONG frameTop = taskbarRect.top +
                          static_cast<LONG>(std::lround(topLeft.Y * scale));
    const LONG frameRight = frameLeft + static_cast<LONG>(
        std::lround(frame.ActualWidth() * scale));
    const LONG frameBottom = frameTop + static_cast<LONG>(
        std::lround(frame.ActualHeight() * scale));

    // Sample the final composed pixels beneath the tray. This includes the
    // user's brightness and blur settings, so contrast follows what is visible.
    RECT sample{
        std::clamp(frameLeft, taskbarRect.left, taskbarRect.right),
        std::clamp(frameTop, taskbarRect.top, taskbarRect.bottom),
        std::clamp(frameRight, taskbarRect.left, taskbarRect.right),
        std::clamp(frameBottom, taskbarRect.top, taskbarRect.bottom)};
    if (sample.right <= sample.left || sample.bottom <= sample.top) {
        return std::nullopt;
    }

    const int sourceWidth = sample.right - sample.left;
    const int sourceHeight = sample.bottom - sample.top;
    const bool horizontal = sourceWidth >= sourceHeight;
    const int outputWidth = horizontal ? 48 : 4;
    const int outputHeight = horizontal ? 4 : 48;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = outputWidth;
    info.bmiHeader.biHeight = -outputHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    void* bits = nullptr;
    HBITMAP bitmap = screen
        ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0)
        : nullptr;
    if (!screen || !memory || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return std::nullopt;
    }

    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    SetStretchBltMode(memory, COLORONCOLOR);
    const BOOL copied = StretchBlt(
        memory, 0, 0, outputWidth, outputHeight, screen,
        sample.left, sample.top, sourceWidth, sourceHeight,
        SRCCOPY | CAPTUREBLT);

    std::optional<double> result;
    if (copied) {
        const auto* pixels = static_cast<const std::uint32_t*>(bits);
        std::vector<double> values;
        values.reserve(static_cast<std::size_t>(outputWidth * outputHeight));
        for (int index = 0; index < outputWidth * outputHeight; ++index) {
            values.push_back(PixelLuminance(pixels[index]));
        }
        auto middle = values.begin() + values.size() / 2;
        std::nth_element(values.begin(), middle, values.end());
        result = *middle;
    }

    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return result;
}

void RefreshForegroundTargets(TrayContext& context,
                              wux::FrameworkElement frame) {
    VisitDescendants(frame, [&](wux::FrameworkElement element) {
        if (auto text = element.try_as<controls::TextBlock>()) {
            const bool known = std::any_of(
                context.textTargets.begin(), context.textTargets.end(),
                [&](const TextForegroundOverride& target) {
                    return target.element.get() == text;
                });
            if (!known) {
                context.textTargets.push_back(
                    {winrt::make_weak(text), text.Foreground()});
            }
        }
        if (auto icon = element.try_as<controls::IconElement>()) {
            const bool known = std::any_of(
                context.iconTargets.begin(), context.iconTargets.end(),
                [&](const IconForegroundOverride& target) {
                    return target.element.get() == icon;
                });
            if (!known) {
                context.iconTargets.push_back(
                    {winrt::make_weak(icon), icon.Foreground()});
            }
        }
    });
}

void RestoreTray(TrayContext& context) {
    if (!context.overridesApplied) {
        return;
    }
    if (auto frame = context.frame.get()) {
        frame.RequestedTheme(context.originalFrameTheme);
    }
    if (auto target = context.themeTarget.get()) {
        target.RequestedTheme(context.originalTargetTheme);
    }
    for (auto& target : context.textTargets) {
        if (auto element = target.element.get()) {
            element.Foreground(target.original);
        }
    }
    for (auto& target : context.iconTargets) {
        if (auto element = target.element.get()) {
            element.Foreground(target.original);
        }
    }
    context.overridesApplied = false;
    context.initialized = false;
}

void ApplyTrayForeground(TrayContext& context, bool light) {
    auto frame = context.frame.get();
    if (!frame) {
        return;
    }
    auto target = context.themeTarget.get();
    if (!target) {
        target = FindDescendantByName(frame, L"SystemTrayFrameGrid");
        if (!target) {
            target = frame;
        }
        context.themeTarget = target;
        context.originalTargetTheme = target.RequestedTheme();
    }

    RefreshForegroundTargets(context, frame);
    const auto theme = light ? wux::ElementTheme::Dark
                             : wux::ElementTheme::Light;
    frame.RequestedTheme(theme);
    target.RequestedTheme(theme);
    const auto color = light ? winrt::Windows::UI::Color{255, 255, 255, 255}
                             : winrt::Windows::UI::Color{255, 18, 18, 18};
    auto brush = media::SolidColorBrush(color);
    for (auto& foreground : context.textTargets) {
        if (auto element = foreground.element.get()) {
            element.Foreground(brush);
        }
    }
    for (auto& foreground : context.iconTargets) {
        if (auto element = foreground.element.get()) {
            element.Foreground(brush);
        }
    }
    context.overridesApplied = true;
    WriteTrayDiagnostic(context);
}

void UpdateTray(TrayContext& context, bool appActive) {
    auto frame = context.frame.get();
    if (!frame) {
        return;
    }
    if (!appActive || !g_contrastSettings.enabled) {
        RestoreTray(context);
        return;
    }

    const auto luminance = SampleTrayLuminance(context, frame);
    if (!luminance) {
        return;
    }
    bool changed = false;
    if (!context.initialized) {
        context.filteredLuminance = *luminance;
        context.lightGlyphs =
            *luminance <
            (g_contrastSettings.lightThreshold +
             g_contrastSettings.darkThreshold) /
                2.0;
        context.initialized = true;
        changed = true;
    } else {
        constexpr double kNewSampleMix = 0.38;
        context.filteredLuminance =
            context.filteredLuminance * (1.0 - kNewSampleMix) +
            *luminance * kNewSampleMix;
        if (context.lightGlyphs &&
            context.filteredLuminance > g_contrastSettings.darkThreshold) {
            context.lightGlyphs = false;
            changed = true;
        } else if (!context.lightGlyphs &&
                   context.filteredLuminance <
                       g_contrastSettings.lightThreshold) {
            context.lightGlyphs = true;
            changed = true;
        }
    }
    if (changed || !context.overridesApplied) {
        ApplyTrayForeground(context, context.lightGlyphs);
    }
}

std::optional<double> SampleIndicatorLuminance(
    RunningIndicatorOverride& context,
    shapes::Shape indicator) {
    if (!context.taskbar || !IsWindow(context.taskbar)) {
        context.taskbar = ResolveTaskbarWindow(indicator);
    }
    if (!context.taskbar) {
        return std::nullopt;
    }
    const auto snapshot = ReadSurface(context.taskbar);
    if (!snapshot || !snapshot->visible || snapshot->pixels.empty()) {
        return std::nullopt;
    }

    const auto root = indicator.XamlRoot();
    const double scale = root
        ? static_cast<double>(root.RasterizationScale())
        : static_cast<double>(WindowDpi(context.taskbar)) / 96.0;
    const auto topLeft =
        indicator.TransformToVisual(nullptr).TransformPoint({0, 0});
    const bool horizontal = snapshot->pixelHeight == 1;
    const double logicalStart = horizontal ? topLeft.X : topLeft.Y;
    const double logicalLength = horizontal
        ? indicator.ActualWidth()
        : indicator.ActualHeight();
    const int available = horizontal
        ? snapshot->pixelWidth
        : snapshot->pixelHeight;
    int start = static_cast<int>(std::floor(logicalStart * scale));
    int end = static_cast<int>(std::ceil(
        (logicalStart + std::max(1.0, logicalLength)) * scale));
    start = std::clamp(start, 0, available);
    end = std::clamp(end, 0, available);
    if (end <= start) {
        return std::nullopt;
    }

    std::vector<double> luminances;
    luminances.reserve(static_cast<std::size_t>(end - start));
    for (int pixel = start; pixel < end; ++pixel) {
        luminances.push_back(PixelLuminance(
            snapshot->pixels[static_cast<std::size_t>(pixel)]));
    }
    auto middle = luminances.begin() + luminances.size() / 2;
    std::nth_element(luminances.begin(), middle, luminances.end());
    return *middle;
}

void RestoreRunningIndicator(RunningIndicatorOverride& context) {
    if (!context.overridden) {
        return;
    }
    if (auto indicator = context.element.get()) {
        indicator.Fill(context.original);
    }
    context.applied = nullptr;
    context.overridden = false;
    context.initialized = false;
}

void UpdateRunningIndicator(
    RunningIndicatorOverride& context,
    bool appActive) {
    auto indicator = context.element.get();
    if (!indicator) {
        return;
    }
    if (!appActive || !g_contrastSettings.enabled) {
        RestoreRunningIndicator(context);
        return;
    }
    const auto luminance = SampleIndicatorLuminance(context, indicator);
    if (!luminance) {
        RestoreRunningIndicator(context);
        return;
    }

    bool changed = false;
    if (!context.initialized) {
        context.filteredLuminance = *luminance;
        context.light =
            *luminance <
            (g_contrastSettings.lightThreshold +
             g_contrastSettings.darkThreshold) /
                2.0;
        context.initialized = true;
        changed = true;
    } else {
        constexpr double kNewSampleMix = 0.38;
        context.filteredLuminance =
            context.filteredLuminance * (1.0 - kNewSampleMix) +
            *luminance * kNewSampleMix;
        if (context.light &&
            context.filteredLuminance > g_contrastSettings.darkThreshold) {
            context.light = false;
            changed = true;
        } else if (!context.light &&
                   context.filteredLuminance <
                       g_contrastSettings.lightThreshold) {
            context.light = true;
            changed = true;
        }
    }

    if (changed || !context.applied) {
        const auto color = context.light
            ? winrt::Windows::UI::Color{255, 255, 255, 255}
            : winrt::Windows::UI::Color{255, 18, 18, 18};
        context.applied = media::SolidColorBrush(color);
    }
    const auto current = indicator.Fill();
    if (winrt::get_abi(current) != winrt::get_abi(context.applied)) {
        indicator.Fill(context.applied);
    }
    context.overridden = true;
}

void UpdateBackgrounds(bool active) {
    if (!g_transparentBrush) {
        g_transparentBrush = media::SolidColorBrush(
            winrt::Windows::UI::Color{0, 0, 0, 0});
    }
    std::erase_if(g_backgrounds, [](const BackgroundOverride& item) {
        return !item.element.get();
    });
    for (auto& item : g_backgrounds) {
        if (auto shape = item.element.get()) {
            if (active) {
                const auto snapshot = ReadSurface(item.taskbar);
                const media::Brush desired = snapshot
                    ? BrushForSurface(item.taskbar, *snapshot)
                    : media::Brush{g_transparentBrush};
                const auto current = shape.Fill();
                if (winrt::get_abi(current) != winrt::get_abi(desired)) {
                    if (item.overridden) {
                        RecordBackgroundGuardCorrection();
                    }
                    g_applyingBackground = true;
                    shape.Fill(desired);
                    g_applyingBackground = false;
                }
                item.overridden = true;
                if (snapshot && snapshot->visible &&
                    !g_surfaceDiagnosticWritten) {
                    const std::wstring path = LocalSettingsPath();
                    if (!path.empty()) {
                        WritePrivateProfileStringW(
                            L"diagnostics",
                            L"surface_bridge",
                            L"native XAML image active",
                            path.c_str());
                    }
                    g_surfaceDiagnosticWritten = true;
                }
            } else if (item.overridden) {
                g_applyingBackground = true;
                shape.Fill(item.original);
                g_applyingBackground = false;
                item.overridden = false;
            }
        }
    }
}

void FlushPendingDiagnosticsReferences();

void Pulse() {
    try {
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastSettingsRead >= 750) {
            g_contrastSettings = ReadContrastSettings();
            g_lastSettingsRead = now;
        }
        const bool active = IsChameleonBarActive();
        UpdateBackgrounds(active);
        std::erase_if(g_trays, [](const std::shared_ptr<TrayContext>& tray) {
            return !tray->frame.get();
        });
        for (const auto& tray : g_trays) {
            UpdateTray(*tray, active);
        }
        std::erase_if(
            g_runningIndicators,
            [](const RunningIndicatorOverride& indicator) {
                return !indicator.element.get();
            });
        for (auto& indicator : g_runningIndicators) {
            UpdateRunningIndicator(indicator, active);
        }
    } catch (...) {
        DebugLog(L"pulse failed");
    }
}

void EnsurePulseTimer() {
    if (g_pulseTimer) {
        return;
    }
    g_pulseTimer = wux::DispatcherTimer();
    g_pulseTimer.Interval(wf::TimeSpan{std::chrono::milliseconds(33)});
    g_pulseToken = g_pulseTimer.Tick(
        [](wf::IInspectable const&, wf::IInspectable const&) {
            Pulse();
            FlushPendingDiagnosticsReferences();
        });
    g_pulseTimer.Start();
}

void RegisterBackground(wux::FrameworkElement element) {
    auto shape = element.try_as<shapes::Shape>();
    if (!shape) {
        return;
    }
    const std::wstring name = element.Name().c_str();
    if (name != L"BackgroundFill" && name != L"BackgroundStroke") {
        return;
    }
    if (!HasAncestorClass(
            element,
            L"Taskbar.TaskbarBackground",
            L"HoverFlyoutBackgroundControl")) {
        return;
    }
    const bool known = std::any_of(
        g_backgrounds.begin(), g_backgrounds.end(),
        [&](const BackgroundOverride& item) {
            return item.element.get() == shape;
        });
    if (!known) {
        g_backgrounds.push_back({
            winrt::make_weak(shape),
            shape.Fill(),
            ResolveTaskbarWindow(element),
            false});
        shape.RegisterPropertyChangedCallback(
            shapes::Shape::FillProperty(),
            [](wux::DependencyObject const&,
               wux::DependencyProperty const&) {
                try {
                    if (!IsChameleonBarActive() || g_applyingBackground) {
                        return;
                    }
                    RecordBackgroundGuardCorrection();
                    UpdateBackgrounds(true);
                } catch (...) {
                    DebugLog(L"background visual-state guard failed");
                }
            });
    }
}

void RegisterTray(wux::FrameworkElement element) {
    if (winrt::get_class_name(element) != L"SystemTray.SystemTrayFrame") {
        return;
    }
    const bool known = std::any_of(
        g_trays.begin(), g_trays.end(),
        [&](const std::shared_ptr<TrayContext>& context) {
            return context->frame.get() == element;
        });
    if (known) {
        return;
    }
    auto context = std::make_shared<TrayContext>();
    context->frame = element;
    context->originalFrameTheme = element.RequestedTheme();
    context->taskbar = ResolveTaskbarWindow(element);
    g_trays.push_back(std::move(context));
}

void RegisterRunningIndicator(wux::FrameworkElement element) {
    if (element.Name() != L"RunningIndicator") {
        return;
    }
    auto indicator = element.try_as<shapes::Shape>();
    if (!indicator ||
        (!HasAncestorClass(element, L"Taskbar.TaskListButton") &&
         !HasAncestorClass(element, L"Taskbar.TaskListButtonPanel") &&
         !HasAncestorClass(
             element, L"Taskbar.TaskListLabeledButtonPanel"))) {
        return;
    }
    const bool known = std::any_of(
        g_runningIndicators.begin(),
        g_runningIndicators.end(),
        [&](const RunningIndicatorOverride& context) {
            return context.element.get() == indicator;
        });
    if (known) {
        return;
    }
    g_runningIndicators.push_back({
        winrt::make_weak(indicator),
        indicator.Fill(),
        nullptr,
        ResolveTaskbarWindow(element)});

    const std::wstring path = LocalSettingsPath();
    if (!path.empty()) {
        const std::wstring count =
            std::to_wstring(g_runningIndicators.size());
        WritePrivateProfileStringW(
            L"diagnostics", L"running_indicator_targets",
            count.c_str(), path.c_str());
    }
}

void HandleElement(wux::FrameworkElement element) {
    RegisterBackground(element);
    RegisterTray(element);
    RegisterRunningIndicator(element);
    if (!g_backgrounds.empty() || !g_trays.empty() ||
        !g_runningIndicators.empty()) {
        EnsurePulseTimer();
        Pulse();
    }
}

// XamlDiagnostics implements this private test interface. Releasing its runtime
// object reference prevents the diagnostics layer from pinning the entire taskbar
// tree after an element is removed.
constexpr GUID kIidXamlDiagnosticsTestHooks = {
    0x735941a2,
    0x3ee3,
    0x495a,
    {0x8d, 0xa9, 0x97, 0x26, 0x27, 0x00, 0x30, 0x75}};

struct IXamlDiagnosticsTestHooks : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(
        InstanceHandle handle, void** dispatcherQueue) = 0;
};

class VisualTreeWatcher : public winrt::implements<
                              VisualTreeWatcher,
                              IVisualTreeServiceCallback2,
                              winrt::non_agile> {
public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
        : diagnostics_(site.as<IXamlDiagnostics>()) {
        diagnostics_->QueryInterface(
            kIidXamlDiagnosticsTestHooks, testHooks_.put_void());
        HANDLE thread = CreateThread(
            nullptr,
            0,
            [](void* parameter) -> DWORD {
                auto* self = static_cast<VisualTreeWatcher*>(parameter);
                const HRESULT status =
                    self->diagnostics_.as<IVisualTreeService3>()
                        ->AdviseVisualTreeChange(self);
                if (FAILED(status)) {
                    DebugLog(L"AdviseVisualTreeChange failed");
                }
                self->Release();
                return 0;
            },
            this,
            0,
            nullptr);
        if (thread) {
            AddRef();
            CloseHandle(thread);
        }
    }

    void Unadvise() {
        diagnostics_.as<IVisualTreeService3>()->UnadviseVisualTreeChange(this);
    }

    void ReleaseDiagnosticsReference(InstanceHandle handle) {
        if (testHooks_ && handle) {
            testHooks_->UnregisterInstance(handle);
        }
    }

    std::vector<InstanceHandle> pendingDiagnosticsReferences_;
    ULONGLONG lastDiagnosticsReferenceTick_ = 0;

private:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation relation,
        VisualElement element,
        VisualMutationType mutationType) noexcept override {
        try {
            if (mutationType == Add) {
                wf::IInspectable inspectable;
                winrt::check_hresult(diagnostics_->GetIInspectableFromHandle(
                    element.Handle,
                    reinterpret_cast<::IInspectable**>(
                        winrt::put_abi(inspectable))));
                if (auto frameworkElement =
                        inspectable.try_as<wux::FrameworkElement>()) {
                    HandleElement(frameworkElement);
                }
            }
            if (element.Handle) {
                pendingDiagnosticsReferences_.push_back(element.Handle);
                if (relation.Parent) {
                    pendingDiagnosticsReferences_.push_back(relation.Parent);
                }
                lastDiagnosticsReferenceTick_ = GetTickCount64();
            }
        } catch (...) {
            DebugLog(L"visual tree callback failed");
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle,
        VisualElementState,
        LPCWSTR) noexcept override {
        return S_OK;
    }

    winrt::com_ptr<IXamlDiagnostics> diagnostics_;
    winrt::com_ptr<IXamlDiagnosticsTestHooks> testHooks_;
};

winrt::com_ptr<VisualTreeWatcher> g_watcher;

void FlushPendingDiagnosticsReferences() {
    if (!g_watcher ||
        g_watcher->pendingDiagnosticsReferences_.empty() ||
        GetTickCount64() - g_watcher->lastDiagnosticsReferenceTick_ < 250) {
        return;
    }
    auto pending = std::move(g_watcher->pendingDiagnosticsReferences_);
    g_watcher->pendingDiagnosticsReferences_.clear();
    std::sort(pending.begin(), pending.end());
    pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
    for (InstanceHandle handle : pending) {
        g_watcher->ReleaseDiagnosticsReference(handle);
    }
}

class ChameleonBarTap : public winrt::implements<
                            ChameleonBarTap,
                            IObjectWithSite,
                            winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        try {
            if (g_watcher) {
                g_watcher->Unadvise();
                g_watcher = nullptr;
            }
            site_.copy_from(site);
            if (site_) {
                // Keep the diagnostics load reference for Explorer's lifetime.
                // Unlike a Windhawk mod, this standalone bridge has no second
                // host-owned module reference to fall back to.
                g_watcher = winrt::make_self<VisualTreeWatcher>(site_);
                if (!g_readyEvent) {
                    g_readyEvent = CreateEventW(
                        nullptr, TRUE, TRUE, kBridgeReadyEventName);
                } else {
                    SetEvent(g_readyEvent);
                }
            } else if (g_readyEvent) {
                ResetEvent(g_readyEvent);
            }
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** result) noexcept override {
        try {
            return site_.as(iid, result);
        } catch (...) {
            return winrt::to_hresult();
        }
    }

private:
    winrt::com_ptr<IUnknown> site_;
};

template <typename T>
class SimpleFactory : public winrt::implements<
                          SimpleFactory<T>,
                          IClassFactory,
                          winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(
        IUnknown* outer,
        REFIID iid,
        void** result) noexcept override {
        try {
            if (outer) {
                return CLASS_E_NOAGGREGATION;
            }
            *result = nullptr;
            return winrt::make<T>().as(iid, result);
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        return S_OK;
    }
};

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_readyEvent) {
            CloseHandle(g_readyEvent);
            g_readyEvent = nullptr;
        }
        if (g_surfaceState) {
            UnmapViewOfFile(g_surfaceState);
            g_surfaceState = nullptr;
        }
        if (g_surfaceMapping) {
            CloseHandle(g_surfaceMapping);
            g_surfaceMapping = nullptr;
        }
    }
    return TRUE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(
    REFCLSID classId,
    REFIID interfaceId,
    void** result) {
    try {
        if (classId != kChameleonBarTap) {
            return CLASS_E_CLASSNOTAVAILABLE;
        }
        *result = nullptr;
        return winrt::make<SimpleFactory<ChameleonBarTap>>()
            .as(interfaceId, result);
    } catch (...) {
        return winrt::to_hresult();
    }
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}
