#include "window_activity.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <unordered_map>

namespace chameleon {
namespace {

std::wstring Lowercase(std::wstring value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

std::wstring ProcessName(
    HWND window,
    std::unordered_map<DWORD, std::wstring>& cache) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId) {
        return {};
    }
    if (const auto iterator = cache.find(processId); iterator != cache.end()) {
        return iterator->second;
    }

    std::wstring result;
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process) {
        std::array<wchar_t, 32768> path{};
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
            std::wstring fullPath(path.data(), length);
            const std::size_t slash = fullPath.find_last_of(L"\\/");
            result = Lowercase(
                slash == std::wstring::npos
                    ? fullPath
                    : fullPath.substr(slash + 1));
        }
        CloseHandle(process);
    }
    cache.emplace(processId, result);
    return result;
}

std::wstring WindowClass(HWND window) {
    std::array<wchar_t, 256> className{};
    GetClassNameW(
        window, className.data(), static_cast<int>(className.size()));
    return className.data();
}

bool IsExcludedClass(const std::wstring& className) {
    constexpr std::array<const wchar_t*, 10> excluded{
        L"Progman",
        L"WorkerW",
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"ChameleonBar.Backdrop",
        L"TaskListThumbnailWnd",
        L"MultitaskingViewFrame",
        L"XamlExplorerHostIslandWindow",
        L"Windows.UI.Composition.DesktopWindowContentBridge",
        L"SysShadow",
    };
    return std::any_of(
        excluded.begin(), excluded.end(),
        [&](const wchar_t* value) { return className == value; });
}

bool IsExcludedShellProcess(const std::wstring& processName) {
    constexpr std::array<const wchar_t*, 13> excluded{
        L"startmenuexperiencehost.exe",
        L"searchhost.exe",
        L"searchapp.exe",
        L"shellexperiencehost.exe",
        L"shellhost.exe",
        L"textinputhost.exe",
        L"lockapp.exe",
        L"widgets.exe",
        L"widgetservice.exe",
        L"sihost.exe",
        L"taskhostw.exe",
        L"ctfmon.exe",
        L"dwm.exe",
    };
    return std::any_of(
        excluded.begin(), excluded.end(),
        [&](const wchar_t* value) { return processName == value; });
}

struct EnumerationContext {
    WindowActivity activity;
    std::unordered_map<DWORD, std::wstring>* processNames = nullptr;
};

// Process names are looked up for every visible window on every scan, so keep
// them between scans. The cache is flushed periodically so a reused process
// id cannot keep a stale name for long.
std::unordered_map<DWORD, std::wstring>& ProcessNameCache() {
    static std::unordered_map<DWORD, std::wstring> cache;
    static ULONGLONG flushedAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - flushedAt > 5000) {
        cache.clear();
        flushedAt = now;
    }
    return cache;
}

BOOL CALLBACK CollectApplicationWindow(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<EnumerationContext*>(parameter);
    WindowActivitySnapshot snapshot;
    snapshot.visible = IsWindowVisible(window) != FALSE;
    snapshot.minimized = IsIconic(window) != FALSE;
    snapshot.hasOwner = GetWindow(window, GW_OWNER) != nullptr;
    snapshot.style = GetWindowLongPtrW(window, GWL_STYLE);
    snapshot.extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    snapshot.className = WindowClass(window);
    snapshot.processName = ProcessName(window, *context.processNames);

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(
            window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        snapshot.cloaked = cloaked != 0;
    }

    BYTE alpha = 255;
    DWORD colorKey = 0;
    DWORD layeredFlags = 0;
    if ((snapshot.extendedStyle & WS_EX_LAYERED) != 0 &&
        GetLayeredWindowAttributes(
            window, &colorKey, &alpha, &layeredFlags) &&
        (layeredFlags & LWA_ALPHA) != 0 && alpha == 0) {
        snapshot.fullyTransparent = true;
    }

    if (FAILED(DwmGetWindowAttribute(
            window,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &snapshot.bounds,
            sizeof(snapshot.bounds)))) {
        GetWindowRect(window, &snapshot.bounds);
    }

    if (IsApplicationWindow(snapshot)) {
        context.activity.applicationWindows.push_back(snapshot.bounds);
    }
    if (IsTaskbarTransientWindow(snapshot)) {
        context.activity.transientWindows.push_back(snapshot.bounds);
    }
    return TRUE;
}

}  // namespace

bool IsApplicationWindow(const WindowActivitySnapshot& window) {
    if (!window.visible || window.minimized || window.cloaked ||
        window.fullyTransparent || (window.style & WS_CHILD) != 0 ||
        (window.extendedStyle & WS_EX_NOACTIVATE) != 0) {
        return false;
    }

    const LONG width = window.bounds.right - window.bounds.left;
    const LONG height = window.bounds.bottom - window.bounds.top;
    if (width < 120 || height < 80 || IsExcludedClass(window.className)) {
        return false;
    }

    const bool explicitAppWindow =
        (window.extendedStyle & WS_EX_APPWINDOW) != 0;
    if ((window.extendedStyle & WS_EX_TOOLWINDOW) != 0 &&
        !explicitAppWindow) {
        return false;
    }
    if (window.hasOwner && !explicitAppWindow) {
        return false;
    }

    const std::wstring processName = Lowercase(window.processName);
    if (processName == L"explorer.exe") {
        return window.className == L"CabinetWClass" ||
               window.className == L"ExploreWClass";
    }
    return !IsExcludedShellProcess(processName);
}

bool IsTaskbarTransientWindow(const WindowActivitySnapshot& window) {
    if (!window.visible || window.minimized || window.cloaked ||
        window.fullyTransparent || (window.style & WS_CHILD) != 0) {
        return false;
    }
    const LONG width = window.bounds.right - window.bounds.left;
    const LONG height = window.bounds.bottom - window.bounds.top;
    if (width < 20 || height < 20) {
        return false;
    }

    if (window.className == L"Progman" || window.className == L"WorkerW" ||
        window.className == L"Shell_TrayWnd" ||
        window.className == L"Shell_SecondaryTrayWnd" ||
        window.className == L"ChameleonBar.Backdrop" ||
        window.className == L"ChameleonBar.Controller" ||
        window.className == L"ChameleonBar.Settings" ||
        window.className.starts_with(L"ShellHandwritingCanvas")) {
        return false;
    }

    constexpr std::array<const wchar_t*, 7> popupClasses{
        L"TaskListThumbnailWnd",
        L"TopLevelWindowForOverflowXamlIsland",
        L"ControlCenterWindow",
        L"NotificationCenterWindow",
        L"XamlExplorerHostIslandWindow",
        L"#32768",
        L"tooltips_class32",
    };
    if (std::any_of(
            popupClasses.begin(), popupClasses.end(),
            [&](const wchar_t* value) { return window.className == value; })) {
        return true;
    }

    const std::wstring processName = Lowercase(window.processName);
    if (IsExcludedShellProcess(processName)) {
        return true;
    }
    const bool popupStyle = (window.style & WS_POPUP) != 0 &&
                            (window.style & WS_CAPTION) == 0;
    const bool transientStyle =
        (window.extendedStyle &
         (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0;
    if (processName == L"explorer.exe") {
        return window.className != L"CabinetWClass" &&
               window.className != L"ExploreWClass" &&
               (popupStyle || transientStyle || window.hasOwner);
    }
    return !IsApplicationWindow(window) &&
           (popupStyle || transientStyle || window.hasOwner);
}

std::vector<RECT> EnumerateVisibleApplicationWindows() {
    return EnumerateWindowActivity().applicationWindows;
}

WindowActivity EnumerateWindowActivity() {
    EnumerationContext context;
    context.processNames = &ProcessNameCache();
    EnumWindows(
        CollectApplicationWindow, reinterpret_cast<LPARAM>(&context));
    return std::move(context.activity);
}

bool SubstantiallyIntersects(const RECT& window, const RECT& monitor) {
    RECT intersection{};
    if (!IntersectRect(&intersection, &window, &monitor)) {
        return false;
    }
    const LONG width = intersection.right - intersection.left;
    const LONG height = intersection.bottom - intersection.top;
    return width >= 80 && height >= 60;
}

bool IsNearTaskbar(
    const RECT& window,
    const RECT& taskbar,
    const RECT& monitor,
    int maximumGap) {
    const LONG horizontalOverlap = std::max<LONG>(
        0,
        std::min(window.right, taskbar.right) -
            std::max(window.left, taskbar.left));
    const LONG verticalOverlap = std::max<LONG>(
        0,
        std::min(window.bottom, taskbar.bottom) -
            std::max(window.top, taskbar.top));
    const int gapLimit = std::max(0, maximumGap);

    const LONG bottomDistance = std::abs(taskbar.bottom - monitor.bottom);
    const LONG topDistance = std::abs(taskbar.top - monitor.top);
    const LONG leftDistance = std::abs(taskbar.left - monitor.left);
    const LONG rightDistance = std::abs(taskbar.right - monitor.right);
    const LONG nearest = std::min(
        std::min(bottomDistance, topDistance),
        std::min(leftDistance, rightDistance));

    if (nearest == bottomDistance) {
        const LONG gap = std::max<LONG>(0, taskbar.top - window.bottom);
        return horizontalOverlap >= 16 && window.bottom <= taskbar.bottom &&
               gap <= gapLimit;
    }
    if (nearest == topDistance) {
        const LONG gap = std::max<LONG>(0, window.top - taskbar.bottom);
        return horizontalOverlap >= 16 && window.top >= taskbar.top &&
               gap <= gapLimit;
    }
    if (nearest == leftDistance) {
        const LONG gap = std::max<LONG>(0, window.left - taskbar.right);
        return verticalOverlap >= 16 && window.left >= taskbar.left &&
               gap <= gapLimit;
    }
    const LONG gap = std::max<LONG>(0, taskbar.left - window.right);
    return verticalOverlap >= 16 && window.right <= taskbar.right &&
           gap <= gapLimit;
}

}  // namespace chameleon
