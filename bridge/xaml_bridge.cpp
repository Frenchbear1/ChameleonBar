// ChameleonBar local Windows 11 XAML bridge.
//
// This file is distributed under GNU GPL v3.0. Its small XAML diagnostics
// attachment layer is derived from Windows 11 Taskbar Styler by m417z:
// https://github.com/ramensoftware/windhawk-mods/blob/main/mods/windows-11-taskbar-styler.wh.cpp
//
// What it does inside Explorer:
//  - Adds one ChameleonBar-owned Rectangle directly above each taskbar's
//    native BackgroundFill and paints the colors published by ChameleonBar.exe
//    into it. Windows never styles that element, so opening Start, Search,
//    Quick Settings or focusing the taskbar cannot override it (the previous
//    approach set BackgroundFill.Fill, which visual states replace).
//  - Keeps the system tray, clock and running-app underlines readable by
//    switching them between light and dark using the published pixels.
//  - Restores everything as soon as ChameleonBar pauses, exits or crashes.
//
// Nothing here captures the screen or touches the disk; all data arrives over
// a small shared-memory mapping.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>

#include "../src/surface_protocol.h"

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace controls = winrt::Windows::UI::Xaml::Controls;
namespace hosting = winrt::Windows::UI::Xaml::Hosting;
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

struct IDesktopWindowXamlSourceNativeAbi : ::IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AttachToWindow(HWND parent) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND* window) = 0;
};

// {3CBCF1BF-2F76-4E9C-96AB-E84B37972554}
constexpr GUID kIidDesktopWindowXamlSourceNative = {
    0x3cbcf1bf,
    0x2f76,
    0x4e9c,
    {0x96, 0xab, 0xe8, 0x4b, 0x37, 0x97, 0x25, 0x54}};

namespace {

using chameleon::kSurfaceClear;
using chameleon::kSurfaceImage;
using chameleon::kSurfaceNative;

constexpr wchar_t kOverlayName[] = L"ChameleonBarSurface";
constexpr auto kActiveInterval = std::chrono::milliseconds(16);
constexpr auto kIdleInterval = std::chrono::milliseconds(250);
constexpr ULONGLONG kTaskbarResolveMilliseconds = 3000;
constexpr ULONGLONG kTrayRefreshMilliseconds = 2000;

HANDLE g_readyEvent;

void DebugLog(const wchar_t* text) {
    OutputDebugStringW(L"ChameleonBar.XamlBridge: ");
    OutputDebugStringW(text);
    OutputDebugStringW(L"\n");
}

bool IsChameleonBarActive() {
    // Re-open every time: the event vanishing is the crash fail-safe, and a
    // cached handle would keep it alive.
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, chameleon::kActiveEventName);
    if (!event) {
        return false;
    }
    const bool active = WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    CloseHandle(event);
    return active;
}

UINT WindowDpi(HWND window) {
    using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow =
        reinterpret_cast<GetDpiForWindowFunction>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    return getDpiForWindow ? getDpiForWindow(window) : 96;
}

bool IsTaskbarWindow(HWND window) {
    wchar_t className[64]{};
    GetClassNameW(window, className, ARRAYSIZE(className));
    return wcscmp(className, L"Shell_TrayWnd") == 0 ||
           wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

// ---------------------------------------------------------------------------
// Shared surface
// ---------------------------------------------------------------------------

HANDLE g_surfaceMapping;
chameleon::SharedSurfaceState* g_surfaceState;
bool g_surfaceWritable;

bool EnsureSurfaceMapping() {
    if (g_surfaceState) {
        return true;
    }
    g_surfaceWritable = true;
    g_surfaceMapping = OpenFileMappingW(
        FILE_MAP_READ | FILE_MAP_WRITE, FALSE, chameleon::kSurfaceMappingName);
    if (!g_surfaceMapping) {
        g_surfaceWritable = false;
        g_surfaceMapping = OpenFileMappingW(
            FILE_MAP_READ, FALSE, chameleon::kSurfaceMappingName);
    }
    if (!g_surfaceMapping) {
        return false;
    }
    g_surfaceState = static_cast<chameleon::SharedSurfaceState*>(MapViewOfFile(
        g_surfaceMapping,
        g_surfaceWritable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ,
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

struct SlotCache {
    HWND taskbar = nullptr;
    ULONGLONG pulse = 0;
    bool found = false;
    LONG mode = kSurfaceNative;
    LONG revision = LONG_MIN;
    int pixelWidth = 0;
    int pixelHeight = 0;
    std::vector<std::uint32_t> pixels;

    bool HasPixels() const {
        return found && !pixels.empty() &&
               static_cast<std::size_t>(pixelWidth) *
                       static_cast<std::size_t>(pixelHeight) ==
                   pixels.size();
    }
    LONG EffectiveMode() const {
        return HasPixels() ? mode : kSurfaceNative;
    }
};

thread_local std::vector<SlotCache> g_slotCache;
thread_local ULONGLONG g_pulseNumber;

void RefreshSlot(SlotCache& cache) {
    if (!EnsureSurfaceMapping() ||
        g_surfaceState->version != chameleon::kSurfaceProtocolVersion) {
        cache.found = false;
        return;
    }
    const auto key = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(cache.taskbar));
    std::vector<std::uint32_t> pixels;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG before = g_surfaceState->sequence;
        MemoryBarrier();
        if ((before & 1) != 0) {
            YieldProcessor();
            continue;
        }
        const chameleon::SharedSurfaceSlot* slot = nullptr;
        for (const auto& candidate : g_surfaceState->slots) {
            if (candidate.taskbarWindow == key) {
                slot = &candidate;
                break;
            }
        }
        bool found = slot != nullptr;
        LONG mode = kSurfaceNative;
        LONG revision = LONG_MIN;
        int width = 0;
        int height = 0;
        bool copied = false;
        if (slot) {
            mode = slot->mode;
            revision = slot->revision;
            width = std::max(0, static_cast<int>(slot->pixelWidth));
            height = std::max(0, static_cast<int>(slot->pixelHeight));
            const int count = std::clamp(
                static_cast<int>(slot->pixelCount),
                0,
                static_cast<int>(chameleon::kMaximumSurfacePixels));
            if (revision != cache.revision) {
                pixels.assign(slot->pixels, slot->pixels + count);
                copied = true;
            }
        }
        MemoryBarrier();
        if (g_surfaceState->sequence != before) {
            continue;
        }
        cache.found = found;
        if (found) {
            cache.mode = mode;
            cache.revision = revision;
            cache.pixelWidth = width;
            cache.pixelHeight = height;
            if (copied) {
                cache.pixels = std::move(pixels);
            }
        }
        return;
    }
    // The writer was busy every time; keep the previous snapshot.
}

// The returned reference stays valid until the next SlotFor() call.
const SlotCache& SlotFor(HWND taskbar) {
    auto iterator = std::find_if(
        g_slotCache.begin(), g_slotCache.end(),
        [taskbar](const SlotCache& cache) { return cache.taskbar == taskbar; });
    if (iterator == g_slotCache.end()) {
        g_slotCache.push_back({});
        g_slotCache.back().taskbar = taskbar;
        iterator = std::prev(g_slotCache.end());
    }
    if (iterator->pulse != g_pulseNumber) {
        RefreshSlot(*iterator);
        iterator->pulse = g_pulseNumber;
    }
    return *iterator;
}

struct ContrastSettings {
    bool enabled = true;
    double lightThreshold = 0.18;
    double darkThreshold = 0.26;
};

ContrastSettings ReadContrastSettings() {
    ContrastSettings settings;
    if (!EnsureSurfaceMapping()) {
        return settings;
    }
    settings.enabled = g_surfaceState->contrastEnabled != 0;
    settings.lightThreshold =
        std::clamp(static_cast<int>(g_surfaceState->lightThresholdPercent), 1, 90) / 100.0;
    settings.darkThreshold =
        std::clamp(static_cast<int>(g_surfaceState->darkThresholdPercent), 2, 95) / 100.0;
    if (settings.darkThreshold <= settings.lightThreshold) {
        settings.darkThreshold = std::min(0.95, settings.lightThreshold + 0.05);
    }
    return settings;
}

// ---------------------------------------------------------------------------
// Element helpers
// ---------------------------------------------------------------------------

wux::FrameworkElement ParentElement(wux::DependencyObject element) {
    return media::VisualTreeHelper::GetParent(element)
        .try_as<wux::FrameworkElement>();
}

wux::FrameworkElement FindAncestorClass(wux::FrameworkElement element,
                                        const wchar_t* className,
                                        const wchar_t* excludedName = nullptr) {
    for (auto parent = ParentElement(element); parent;
         parent = ParentElement(parent)) {
        if (excludedName && parent.Name() == excludedName) {
            return nullptr;
        }
        if (winrt::get_class_name(parent) == className) {
            return parent;
        }
    }
    return nullptr;
}

bool HasAncestorClass(wux::FrameworkElement element, const wchar_t* className) {
    return FindAncestorClass(element, className) != nullptr;
}

void VisitDescendants(
    wux::DependencyObject element,
    const std::function<void(wux::FrameworkElement)>& callback) {
    const int count = media::VisualTreeHelper::GetChildrenCount(element);
    for (int index = 0; index < count; ++index) {
        auto child = media::VisualTreeHelper::GetChild(element, index);
        if (auto framework = child.try_as<wux::FrameworkElement>()) {
            callback(framework);
        }
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

// Remembers a property's local value so it can be restored exactly, including
// "never set" (which must be cleared, not set to the current effective value).
struct SavedLocalValue {
    bool saved = false;
    wf::IInspectable value{nullptr};

    void Save(wux::DependencyObject const& element,
              wux::DependencyProperty const& property) {
        if (!saved) {
            value = element.ReadLocalValue(property);
            saved = true;
        }
    }
    void Restore(wux::DependencyObject const& element,
                 wux::DependencyProperty const& property) {
        if (!saved) {
            return;
        }
        if (!value || value == wux::DependencyProperty::UnsetValue()) {
            element.ClearValue(property);
        } else {
            element.SetValue(property, value);
        }
        saved = false;
        value = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Taskbar window resolution
// ---------------------------------------------------------------------------

struct XamlSourceRecord {
    winrt::weak_ref<hosting::DesktopWindowXamlSource> source;
    HWND taskbar = nullptr;
};

thread_local std::vector<XamlSourceRecord> g_xamlSources;

void RegisterXamlSource(hosting::DesktopWindowXamlSource const& source) {
    winrt::com_ptr<IDesktopWindowXamlSourceNativeAbi> native;
    if (FAILED(reinterpret_cast<::IUnknown*>(winrt::get_abi(source))
                   ->QueryInterface(kIidDesktopWindowXamlSourceNative,
                                    native.put_void()))) {
        return;
    }
    HWND island = nullptr;
    if (FAILED(native->get_WindowHandle(&island)) || !island) {
        return;
    }
    HWND root = GetAncestor(island, GA_ROOT);
    if (!root || !IsTaskbarWindow(root)) {
        return;
    }
    std::erase_if(g_xamlSources, [&](const XamlSourceRecord& record) {
        auto alive = record.source.get();
        return !alive || alive == source;
    });
    g_xamlSources.push_back({winrt::make_weak(source), root});
}

HWND ResolveTaskbarWindowUncached(wux::XamlRoot const& root);

// Every tray, underline and background in one taskbar shares a XamlRoot, so
// resolve each root once and reuse the answer for a few seconds.
struct RootResolution {
    void* root = nullptr;
    HWND taskbar = nullptr;
    ULONGLONG resolvedAt = 0;
};
thread_local std::vector<RootResolution> g_rootResolutions;

HWND ResolveTaskbarWindow(wux::UIElement const& element) {
    const auto root = element.XamlRoot();
    if (!root) {
        return nullptr;
    }
    void* key = winrt::get_abi(root);
    const ULONGLONG now = GetTickCount64();
    for (auto& entry : g_rootResolutions) {
        if (entry.root == key) {
            if (entry.taskbar && IsWindow(entry.taskbar) &&
                now - entry.resolvedAt < kTaskbarResolveMilliseconds) {
                return entry.taskbar;
            }
            entry.taskbar = ResolveTaskbarWindowUncached(root);
            entry.resolvedAt = now;
            return entry.taskbar;
        }
    }
    std::erase_if(g_rootResolutions, [now](const RootResolution& entry) {
        return now - entry.resolvedAt > 60000;
    });
    const HWND taskbar = ResolveTaskbarWindowUncached(root);
    g_rootResolutions.push_back({key, taskbar, now});
    return taskbar;
}

HWND ResolveTaskbarWindowUncached(wux::XamlRoot const& root) {

    // Exact: the DesktopWindowXamlSource that hosts this XamlRoot.
    for (const auto& record : g_xamlSources) {
        if (!IsWindow(record.taskbar)) {
            continue;
        }
        auto source = record.source.get();
        if (!source) {
            continue;
        }
        if (auto content = source.Content()) {
            if (auto contentRoot = content.XamlRoot();
                contentRoot && contentRoot == root) {
                return record.taskbar;
            }
        }
    }

    // Fallback: the taskbar in this process whose size and DPI match.
    const auto rootSize = root.Size();
    const double scale = root.RasterizationScale();
    struct Search {
        int expectedWidth;
        int expectedHeight;
        UINT expectedDpi;
        HWND best = nullptr;
        long long bestScore = LLONG_MAX;
    } search{
        static_cast<int>(std::lround(rootSize.Width * scale)),
        static_cast<int>(std::lround(rootSize.Height * scale)),
        static_cast<UINT>(std::lround(scale * 96.0)),
    };

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
            long long score = std::abs(width - search->expectedWidth) +
                              std::abs(height - search->expectedHeight);
            if (WindowDpi(window) != search->expectedDpi) {
                score += 10000;
            }
            if (score < search->bestScore) {
                search->bestScore = score;
                search->best = window;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}

// ---------------------------------------------------------------------------
// Background surface
// ---------------------------------------------------------------------------

struct SurfaceBrushState {
    HWND taskbar = nullptr;
    int pixelWidth = 0;
    int pixelHeight = 0;
    LONG revision = LONG_MIN;
    imaging::WriteableBitmap bitmap{nullptr};
    media::ImageBrush brush{nullptr};
};

thread_local std::vector<SurfaceBrushState> g_surfaceBrushes;
thread_local media::SolidColorBrush g_transparentBrush{nullptr};
thread_local bool g_applying;

media::Brush TransparentBrush() {
    if (!g_transparentBrush) {
        g_transparentBrush = media::SolidColorBrush(
            winrt::Windows::UI::Color{0, 0, 0, 0});
    }
    return g_transparentBrush;
}

media::Brush BrushForSlot(HWND taskbar, const SlotCache& slot) {
    auto iterator = std::find_if(
        g_surfaceBrushes.begin(), g_surfaceBrushes.end(),
        [taskbar](const SurfaceBrushState& state) { return state.taskbar == taskbar; });
    if (iterator == g_surfaceBrushes.end()) {
        g_surfaceBrushes.push_back({});
        g_surfaceBrushes.back().taskbar = taskbar;
        iterator = std::prev(g_surfaceBrushes.end());
    }
    auto& state = *iterator;
    if (!state.bitmap || state.pixelWidth != slot.pixelWidth ||
        state.pixelHeight != slot.pixelHeight) {
        state.pixelWidth = slot.pixelWidth;
        state.pixelHeight = slot.pixelHeight;
        state.revision = LONG_MIN;
        state.bitmap = imaging::WriteableBitmap(slot.pixelWidth, slot.pixelHeight);
        state.brush = media::ImageBrush();
        state.brush.ImageSource(state.bitmap);
        state.brush.Stretch(media::Stretch::Fill);
    }
    if (state.revision != slot.revision) {
        const auto pixelBuffer = state.bitmap.PixelBuffer();
        winrt::com_ptr<IBufferByteAccess> byteAccess;
        winrt::check_hresult(
            reinterpret_cast<::IUnknown*>(winrt::get_abi(pixelBuffer))
                ->QueryInterface(kIidBufferByteAccess, byteAccess.put_void()));
        std::uint8_t* destination = nullptr;
        winrt::check_hresult(byteAccess->Buffer(&destination));
        std::memcpy(destination, slot.pixels.data(),
                    slot.pixels.size() * sizeof(std::uint32_t));
        state.bitmap.Invalidate();
        state.revision = slot.revision;
    }
    return state.brush;
}

struct BackgroundTarget {
    winrt::weak_ref<shapes::Shape> fill;
    winrt::weak_ref<wux::FrameworkElement> background;  // Taskbar.TaskbarBackground
    winrt::weak_ref<shapes::Shape> stroke;
    shapes::Rectangle overlay{nullptr};
    SavedLocalValue fillOpacity;
    SavedLocalValue strokeOpacity;
    SavedLocalValue directFill;  // only when the overlay cannot be inserted
    bool strokeCallbackRegistered = false;
    bool hidingNative = false;
    HWND taskbar = nullptr;
    ULONGLONG resolvedAt = 0;
    LONG mode = kSurfaceNative;
};

thread_local std::vector<std::shared_ptr<BackgroundTarget>> g_backgrounds;

void KeepTransparent(wux::DependencyObject const& sender,
                     std::weak_ptr<BackgroundTarget> const& weak) {
    if (g_applying) {
        return;
    }
    auto target = weak.lock();
    if (!target || !target->hidingNative) {
        return;
    }
    if (auto element = sender.try_as<wux::UIElement>(); element && element.Opacity() != 0.0) {
        g_applying = true;
        element.Opacity(0.0);
        g_applying = false;
    }
}

shapes::Shape FindStroke(BackgroundTarget& target) {
    if (auto stroke = target.stroke.get()) {
        return stroke;
    }
    auto background = target.background.get();
    if (!background) {
        return nullptr;
    }
    auto stroke = FindDescendantByName(background, L"BackgroundStroke")
                      .try_as<shapes::Shape>();
    if (stroke) {
        target.stroke = winrt::make_weak(stroke);
        target.strokeOpacity = {};
        target.strokeCallbackRegistered = false;
    }
    return stroke;
}

void CopyLayout(shapes::Shape const& fill, shapes::Rectangle const& overlay) {
    overlay.Margin(fill.Margin());
    overlay.HorizontalAlignment(fill.HorizontalAlignment());
    overlay.VerticalAlignment(fill.VerticalAlignment());
    overlay.Width(fill.Width());
    overlay.Height(fill.Height());
    controls::Grid::SetRow(overlay, controls::Grid::GetRow(fill));
    controls::Grid::SetColumn(overlay, controls::Grid::GetColumn(fill));
    controls::Grid::SetRowSpan(overlay, controls::Grid::GetRowSpan(fill));
    controls::Grid::SetColumnSpan(overlay, controls::Grid::GetColumnSpan(fill));
    controls::Canvas::SetZIndex(overlay, controls::Canvas::GetZIndex(fill));
    if (auto rectangle = fill.try_as<shapes::Rectangle>()) {
        overlay.RadiusX(rectangle.RadiusX());
        overlay.RadiusY(rectangle.RadiusY());
    }
}

void DetachOverlay(BackgroundTarget& target) {
    if (!target.overlay) {
        return;
    }
    if (auto panel = media::VisualTreeHelper::GetParent(target.overlay)
                         .try_as<controls::Panel>()) {
        uint32_t index = 0;
        if (panel.Children().IndexOf(target.overlay, index)) {
            panel.Children().RemoveAt(index);
        }
    }
}

// Places the overlay immediately above the native fill. Returns false when
// the fill's parent is not a panel (then the direct-fill fallback is used).
bool EnsureOverlay(BackgroundTarget& target, shapes::Shape const& fill) {
    auto panel = media::VisualTreeHelper::GetParent(fill).try_as<controls::Panel>();
    if (!panel) {
        return false;
    }
    auto children = panel.Children();
    uint32_t fillIndex = 0;
    if (!children.IndexOf(fill, fillIndex)) {
        return false;
    }
    if (!target.overlay) {
        target.overlay = shapes::Rectangle();
        target.overlay.Name(kOverlayName);
        target.overlay.IsHitTestVisible(false);
        target.overlay.Fill(TransparentBrush());
    }
    uint32_t overlayIndex = 0;
    if (children.IndexOf(target.overlay, overlayIndex)) {
        if (overlayIndex == fillIndex + 1) {
            return true;
        }
        children.RemoveAt(overlayIndex);
        if (!children.IndexOf(fill, fillIndex)) {
            return false;
        }
    } else {
        DetachOverlay(target);  // it may still sit in a recycled template
    }
    CopyLayout(fill, target.overlay);
    children.InsertAt(fillIndex + 1, target.overlay);
    return true;
}

void HideNative(BackgroundTarget& target,
                std::shared_ptr<BackgroundTarget> const& self,
                shapes::Shape const& fill) {
    target.hidingNative = true;
    target.fillOpacity.Save(fill, wux::UIElement::OpacityProperty());
    if (fill.Opacity() != 0.0) {
        fill.Opacity(0.0);
    }
    if (auto stroke = FindStroke(target)) {
        if (!target.strokeCallbackRegistered) {
            std::weak_ptr<BackgroundTarget> weak = self;
            stroke.RegisterPropertyChangedCallback(
                wux::UIElement::OpacityProperty(),
                [weak](wux::DependencyObject const& sender,
                       wux::DependencyProperty const&) {
                    try {
                        KeepTransparent(sender, weak);
                    } catch (...) {
                    }
                });
            target.strokeCallbackRegistered = true;
        }
        target.strokeOpacity.Save(stroke, wux::UIElement::OpacityProperty());
        if (stroke.Opacity() != 0.0) {
            stroke.Opacity(0.0);
        }
    }
}

void RestoreTarget(BackgroundTarget& target) {
    target.hidingNative = false;
    target.mode = kSurfaceNative;
    DetachOverlay(target);
    if (auto fill = target.fill.get()) {
        target.fillOpacity.Restore(fill, wux::UIElement::OpacityProperty());
        target.directFill.Restore(fill, shapes::Shape::FillProperty());
    }
    if (auto stroke = target.stroke.get()) {
        target.strokeOpacity.Restore(stroke, wux::UIElement::OpacityProperty());
    }
}

void UpdateTarget(std::shared_ptr<BackgroundTarget> const& self, bool active) {
    BackgroundTarget& target = *self;
    auto fill = target.fill.get();
    if (!fill) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!target.taskbar || !IsWindow(target.taskbar) ||
        now - target.resolvedAt > kTaskbarResolveMilliseconds) {
        target.taskbar = ResolveTaskbarWindow(fill);
        target.resolvedAt = now;
    }

    LONG mode = kSurfaceNative;
    const SlotCache* slot = nullptr;
    if (active && target.taskbar) {
        slot = &SlotFor(target.taskbar);
        mode = slot->EffectiveMode();
    }
    if (mode == kSurfaceNative) {
        if (target.mode != kSurfaceNative || target.hidingNative) {
            RestoreTarget(target);
        }
        return;
    }

    const media::Brush brush = mode == kSurfaceImage
        ? BrushForSlot(target.taskbar, *slot)
        : TransparentBrush();
    g_applying = true;
    if (EnsureOverlay(target, fill)) {
        target.directFill.Restore(fill, shapes::Shape::FillProperty());
        if (target.overlay.Fill() != brush) {
            target.overlay.Fill(brush);
        }
        HideNative(target, self, fill);
    } else {
        // Fallback for an unexpected template: paint the native fill itself.
        target.directFill.Save(fill, shapes::Shape::FillProperty());
        if (fill.Fill() != brush) {
            fill.Fill(brush);
        }
        target.hidingNative = false;
        if (auto stroke = FindStroke(target)) {
            target.strokeOpacity.Save(stroke, wux::UIElement::OpacityProperty());
            if (stroke.Opacity() != 0.0) {
                stroke.Opacity(0.0);
            }
        }
    }
    g_applying = false;
    target.mode = mode;
}

void RegisterBackground(wux::FrameworkElement element) {
    auto shape = element.try_as<shapes::Shape>();
    if (!shape || element.Name() != L"BackgroundFill") {
        return;
    }
    auto background = FindAncestorClass(
        element, L"Taskbar.TaskbarBackground", L"HoverFlyoutBackgroundControl");
    if (!background) {
        return;
    }
    const bool known = std::any_of(
        g_backgrounds.begin(), g_backgrounds.end(),
        [&](const std::shared_ptr<BackgroundTarget>& item) {
            return item->fill.get() == shape;
        });
    if (known) {
        return;
    }
    auto target = std::make_shared<BackgroundTarget>();
    target->fill = winrt::make_weak(shape);
    target->background = winrt::make_weak(background);
    std::weak_ptr<BackgroundTarget> weak = target;
    shape.RegisterPropertyChangedCallback(
        wux::UIElement::OpacityProperty(),
        [weak](wux::DependencyObject const& sender, wux::DependencyProperty const&) {
            try {
                KeepTransparent(sender, weak);
            } catch (...) {
            }
        });
    g_backgrounds.push_back(std::move(target));
}

// ---------------------------------------------------------------------------
// Adaptive contrast
// ---------------------------------------------------------------------------

thread_local ContrastSettings g_contrast;

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

// Median luminance of the published pixels directly beneath an element.
std::optional<double> LuminanceBeneath(wux::FrameworkElement const& element,
                                       const SlotCache& slot) {
    if (!slot.HasPixels()) {
        return std::nullopt;
    }
    const auto root = element.XamlRoot();
    if (!root) {
        return std::nullopt;
    }
    const double scale = root.RasterizationScale();
    const auto topLeft = element.TransformToVisual(nullptr).TransformPoint({0, 0});
    const bool horizontal = slot.pixelHeight == 1;
    const double logicalStart = horizontal ? topLeft.X : topLeft.Y;
    const double logicalLength =
        horizontal ? element.ActualWidth() : element.ActualHeight();
    const int available = horizontal ? slot.pixelWidth : slot.pixelHeight;
    int start = static_cast<int>(std::floor(logicalStart * scale));
    int end = static_cast<int>(
        std::ceil((logicalStart + std::max(1.0, logicalLength)) * scale));
    start = std::clamp(start, 0, available);
    end = std::clamp(end, 0, available);
    if (end <= start) {
        return std::nullopt;
    }
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(end - start));
    for (int pixel = start; pixel < end; ++pixel) {
        values.push_back(PixelLuminance(slot.pixels[static_cast<std::size_t>(pixel)]));
    }
    auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

struct Hysteresis {
    bool initialized = false;
    bool light = false;
    double filtered = 0.0;

    // Returns true when the light/dark decision changed.
    bool Update(double luminance) {
        if (!initialized) {
            filtered = luminance;
            light = luminance < (g_contrast.lightThreshold + g_contrast.darkThreshold) / 2.0;
            initialized = true;
            return true;
        }
        constexpr double kNewSampleMix = 0.38;
        filtered = filtered * (1.0 - kNewSampleMix) + luminance * kNewSampleMix;
        if (light && filtered > g_contrast.darkThreshold) {
            light = false;
            return true;
        }
        if (!light && filtered < g_contrast.lightThreshold) {
            light = true;
            return true;
        }
        return false;
    }
};

struct TextForegroundOverride {
    winrt::weak_ref<controls::TextBlock> element;
    SavedLocalValue original;
};

struct IconForegroundOverride {
    winrt::weak_ref<controls::IconElement> element;
    SavedLocalValue original;
};

struct TrayContext {
    winrt::weak_ref<wux::FrameworkElement> frame;
    winrt::weak_ref<wux::FrameworkElement> themeTarget;
    HWND taskbar = nullptr;
    ULONGLONG resolvedAt = 0;
    ULONGLONG refreshedAt = 0;
    SavedLocalValue frameTheme;
    SavedLocalValue targetTheme;
    Hysteresis decision;
    bool applied = false;
    std::vector<TextForegroundOverride> textTargets;
    std::vector<IconForegroundOverride> iconTargets;
};

thread_local std::vector<std::shared_ptr<TrayContext>> g_trays;

media::SolidColorBrush GlyphBrush(bool light) {
    thread_local media::SolidColorBrush lightBrush{nullptr};
    thread_local media::SolidColorBrush darkBrush{nullptr};
    auto& brush = light ? lightBrush : darkBrush;
    if (!brush) {
        brush = media::SolidColorBrush(
            light ? winrt::Windows::UI::Color{255, 255, 255, 255}
                  : winrt::Windows::UI::Color{255, 18, 18, 18});
    }
    return brush;
}

void RestoreTray(TrayContext& context) {
    if (context.applied) {
        if (auto frame = context.frame.get()) {
            context.frameTheme.Restore(frame, wux::FrameworkElement::RequestedThemeProperty());
        }
        if (auto target = context.themeTarget.get()) {
            context.targetTheme.Restore(target, wux::FrameworkElement::RequestedThemeProperty());
        }
        for (auto& target : context.textTargets) {
            if (auto element = target.element.get()) {
                target.original.Restore(element, controls::TextBlock::ForegroundProperty());
            }
        }
        for (auto& target : context.iconTargets) {
            if (auto element = target.element.get()) {
                target.original.Restore(element, controls::IconElement::ForegroundProperty());
            }
        }
    }
    context.textTargets.clear();
    context.iconTargets.clear();
    context.applied = false;
    context.decision = {};
}

void ApplyTrayForeground(TrayContext& context, bool refreshTargets) {
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
        context.targetTheme = {};
    }
    const bool light = context.decision.light;
    const auto theme = light ? wux::ElementTheme::Dark : wux::ElementTheme::Light;
    context.frameTheme.Save(frame, wux::FrameworkElement::RequestedThemeProperty());
    context.targetTheme.Save(target, wux::FrameworkElement::RequestedThemeProperty());
    if (frame.RequestedTheme() != theme) {
        frame.RequestedTheme(theme);
    }
    if (target.RequestedTheme() != theme) {
        target.RequestedTheme(theme);
    }

    if (refreshTargets) {
        std::erase_if(context.textTargets, [](const TextForegroundOverride& item) {
            return !item.element.get();
        });
        std::erase_if(context.iconTargets, [](const IconForegroundOverride& item) {
            return !item.element.get();
        });
        VisitDescendants(frame, [&](wux::FrameworkElement element) {
            if (auto text = element.try_as<controls::TextBlock>()) {
                const bool known = std::any_of(
                    context.textTargets.begin(), context.textTargets.end(),
                    [&](const TextForegroundOverride& item) { return item.element.get() == text; });
                if (!known) {
                    context.textTargets.push_back({winrt::make_weak(text), {}});
                }
            } else if (auto icon = element.try_as<controls::IconElement>()) {
                const bool known = std::any_of(
                    context.iconTargets.begin(), context.iconTargets.end(),
                    [&](const IconForegroundOverride& item) { return item.element.get() == icon; });
                if (!known) {
                    context.iconTargets.push_back({winrt::make_weak(icon), {}});
                }
            }
        });
    }

    const auto brush = GlyphBrush(light);
    for (auto& item : context.textTargets) {
        if (auto element = item.element.get()) {
            item.original.Save(element, controls::TextBlock::ForegroundProperty());
            if (element.Foreground() != brush) {
                element.Foreground(brush);
            }
        }
    }
    for (auto& item : context.iconTargets) {
        if (auto element = item.element.get()) {
            item.original.Save(element, controls::IconElement::ForegroundProperty());
            if (element.Foreground() != brush) {
                element.Foreground(brush);
            }
        }
    }
    context.applied = true;
}

void UpdateTray(TrayContext& context, bool active) {
    auto frame = context.frame.get();
    if (!frame) {
        return;
    }
    if (!active || !g_contrast.enabled) {
        RestoreTray(context);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!context.taskbar || !IsWindow(context.taskbar) ||
        now - context.resolvedAt > kTaskbarResolveMilliseconds) {
        context.taskbar = ResolveTaskbarWindow(frame);
        context.resolvedAt = now;
    }
    if (!context.taskbar) {
        return;
    }
    const SlotCache& slot = SlotFor(context.taskbar);
    if (slot.EffectiveMode() == kSurfaceNative) {
        RestoreTray(context);
        return;
    }
    const auto luminance = LuminanceBeneath(frame, slot);
    if (!luminance) {
        return;
    }
    const bool changed = context.decision.Update(*luminance);
    const bool refresh = now - context.refreshedAt > kTrayRefreshMilliseconds;
    if (changed || !context.applied || refresh) {
        ApplyTrayForeground(context, changed || !context.applied || refresh);
        context.refreshedAt = now;
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
    if (!known) {
        auto context = std::make_shared<TrayContext>();
        context->frame = element;
        g_trays.push_back(std::move(context));
    }
}

struct RunningIndicatorOverride {
    winrt::weak_ref<shapes::Shape> element;
    SavedLocalValue original;
    HWND taskbar = nullptr;
    ULONGLONG resolvedAt = 0;
    Hysteresis decision;
    ULONGLONG appliedAt = 0;
    bool overridden = false;
};

thread_local std::vector<RunningIndicatorOverride> g_runningIndicators;

void RestoreRunningIndicator(RunningIndicatorOverride& context) {
    if (context.overridden) {
        if (auto indicator = context.element.get()) {
            context.original.Restore(indicator, shapes::Shape::FillProperty());
        }
    }
    context.overridden = false;
    context.decision = {};
}

void UpdateRunningIndicator(RunningIndicatorOverride& context, bool active) {
    auto indicator = context.element.get();
    if (!indicator) {
        return;
    }
    if (!active || !g_contrast.enabled) {
        RestoreRunningIndicator(context);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!context.taskbar || !IsWindow(context.taskbar) ||
        now - context.resolvedAt > kTaskbarResolveMilliseconds) {
        context.taskbar = ResolveTaskbarWindow(indicator);
        context.resolvedAt = now;
    }
    if (!context.taskbar) {
        RestoreRunningIndicator(context);
        return;
    }
    const SlotCache& slot = SlotFor(context.taskbar);
    const auto luminance = slot.EffectiveMode() == kSurfaceNative
        ? std::nullopt
        : LuminanceBeneath(indicator, slot);
    if (!luminance) {
        RestoreRunningIndicator(context);
        return;
    }
    const bool changed = context.decision.Update(*luminance);
    const auto brush = GlyphBrush(context.decision.light);
    context.original.Save(indicator, shapes::Shape::FillProperty());
    // Windows restyles underlines when an app gains or loses focus. Re-apply
    // after that at most once a second instead of fighting it every frame,
    // which kept Explorer's taskbar thread busy.
    if (changed || !context.overridden ||
        (now - context.appliedAt > 1000 && indicator.Fill() != brush)) {
        indicator.Fill(brush);
        context.appliedAt = now;
    }
    context.overridden = true;
}

void RegisterRunningIndicator(wux::FrameworkElement element) {
    if (element.Name() != L"RunningIndicator") {
        return;
    }
    auto indicator = element.try_as<shapes::Shape>();
    if (!indicator ||
        (!HasAncestorClass(element, L"Taskbar.TaskListButton") &&
         !HasAncestorClass(element, L"Taskbar.TaskListButtonPanel") &&
         !HasAncestorClass(element, L"Taskbar.TaskListLabeledButtonPanel"))) {
        return;
    }
    const bool known = std::any_of(
        g_runningIndicators.begin(), g_runningIndicators.end(),
        [&](const RunningIndicatorOverride& context) {
            return context.element.get() == indicator;
        });
    if (!known) {
        RunningIndicatorOverride context;
        context.element = winrt::make_weak(indicator);
        g_runningIndicators.push_back(std::move(context));
    }
}

// ---------------------------------------------------------------------------
// Pulse
// ---------------------------------------------------------------------------

thread_local wux::DispatcherTimer g_pulseTimer{nullptr};
thread_local bool g_inPulse;
thread_local ULONGLONG g_lastPulseTick;
thread_local bool g_wasActive;

void FlushPendingDiagnosticsReferences();

void Pulse() {
    if (g_inPulse) {
        return;
    }
    g_inPulse = true;
    ++g_pulseNumber;
    LARGE_INTEGER pulseStart{};
    QueryPerformanceCounter(&pulseStart);
    const ULONGLONG pulseTick = GetTickCount64();
    const ULONGLONG gap = g_lastPulseTick ? pulseTick - g_lastPulseTick : 0;
    g_lastPulseTick = pulseTick;
    std::erase_if(g_slotCache, [](const SlotCache& cache) {
        return !IsWindow(cache.taskbar);
    });
    std::erase_if(g_surfaceBrushes, [](const SurfaceBrushState& state) {
        return !IsWindow(state.taskbar);
    });
    const bool active = IsChameleonBarActive();
    if (active) {
        g_contrast = ReadContrastSettings();
    }

    int surfaces = 0;
    std::erase_if(g_backgrounds, [](const std::shared_ptr<BackgroundTarget>& item) {
        return !item->fill.get();
    });
    for (const auto& target : g_backgrounds) {
        try {
            UpdateTarget(target, active);
            if (target->mode != kSurfaceNative) {
                ++surfaces;
            }
        } catch (...) {
            g_applying = false;
            DebugLog(L"background update failed");
        }
    }

    int trays = 0;
    std::erase_if(g_trays, [](const std::shared_ptr<TrayContext>& tray) {
        return !tray->frame.get();
    });
    for (const auto& tray : g_trays) {
        try {
            UpdateTray(*tray, active);
            trays += tray->applied ? 1 : 0;
        } catch (...) {
            DebugLog(L"tray update failed");
        }
    }

    int indicators = 0;
    std::erase_if(g_runningIndicators, [](const RunningIndicatorOverride& item) {
        return !item.element.get();
    });
    for (auto& indicator : g_runningIndicators) {
        try {
            UpdateRunningIndicator(indicator, active);
            indicators += indicator.overridden ? 1 : 0;
        } catch (...) {
            DebugLog(L"indicator update failed");
        }
    }

    if (active && g_surfaceState && g_surfaceWritable) {
        LARGE_INTEGER pulseEnd{};
        LARGE_INTEGER frequency{};
        QueryPerformanceCounter(&pulseEnd);
        QueryPerformanceFrequency(&frequency);
        const LONG duration = frequency.QuadPart
            ? static_cast<LONG>((pulseEnd.QuadPart - pulseStart.QuadPart) * 1000 / frequency.QuadPart)
            : 0;
        // Gaps while idle (250 ms timer) are expected; only measure while active.
        if (g_wasActive && gap > 0 &&
            static_cast<LONG>(std::min<ULONGLONG>(gap, LONG_MAX)) > g_surfaceState->bridgeMaxGapMs) {
            g_surfaceState->bridgeMaxGapMs = static_cast<LONG>(std::min<ULONGLONG>(gap, LONG_MAX));
        }
        if (duration > g_surfaceState->bridgeSlowestPulseMs) {
            g_surfaceState->bridgeSlowestPulseMs = duration;
        }
        InterlockedIncrement(&g_surfaceState->bridgePulses);
        g_surfaceState->bridgeBuild = chameleon::kBridgeBuild;
        g_surfaceState->bridgeSurfaces = surfaces;
        g_surfaceState->bridgeTrays = trays;
        g_surfaceState->bridgeIndicators = static_cast<LONG>(indicators);
        MemoryBarrier();
        g_surfaceState->bridgeHeartbeat = static_cast<LONG>(GetTickCount() | 1u);
    }

    if (g_pulseTimer && active != g_wasActive) {
        g_pulseTimer.Interval(active ? wf::TimeSpan{kActiveInterval}
                                     : wf::TimeSpan{kIdleInterval});
    }
    g_wasActive = active;
    g_inPulse = false;
}

void EnsurePulseTimer() {
    if (g_pulseTimer) {
        return;
    }
    g_pulseTimer = wux::DispatcherTimer();
    g_pulseTimer.Interval(wf::TimeSpan{kActiveInterval});
    g_wasActive = true;
    g_pulseTimer.Tick([](wf::IInspectable const&, wf::IInspectable const&) {
        try {
            Pulse();
        } catch (...) {
            g_inPulse = false;
            DebugLog(L"pulse failed");
        }
        FlushPendingDiagnosticsReferences();
    });
    g_pulseTimer.Start();
}

void HandleElement(wux::FrameworkElement element) {
    if (element.Name() == kOverlayName) {
        return;
    }
    RegisterBackground(element);
    RegisterTray(element);
    RegisterRunningIndicator(element);
    if (!g_backgrounds.empty() || !g_trays.empty() || !g_runningIndicators.empty()) {
        // Never modify the tree from inside the diagnostics callback: the
        // timer applies everything on the next tick (at most 16 ms later).
        EnsurePulseTimer();
    }
}

// ---------------------------------------------------------------------------
// XAML diagnostics attachment
// ---------------------------------------------------------------------------

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
                    reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable))));
                if (auto source = inspectable.try_as<hosting::DesktopWindowXamlSource>()) {
                    RegisterXamlSource(source);
                } else if (auto frameworkElement =
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
                g_watcher = winrt::make_self<VisualTreeWatcher>(site_);
                if (!g_readyEvent) {
                    g_readyEvent = CreateEventW(
                        nullptr, TRUE, TRUE, chameleon::kBridgeReadyEventName);
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

STDAPI DllGetClassObject(
    REFCLSID classId,
    REFIID interfaceId,
    LPVOID* result) {
    try {
        if (classId != chameleon::kChameleonBarTapClsid) {
            return CLASS_E_CLASSNOTAVAILABLE;
        }
        *result = nullptr;
        return winrt::make<SimpleFactory<ChameleonBarTap>>()
            .as(interfaceId, result);
    } catch (...) {
        return winrt::to_hresult();
    }
}

STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}
