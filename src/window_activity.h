#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace chameleon {

struct WindowActivitySnapshot {
    bool visible = false;
    bool minimized = false;
    bool cloaked = false;
    bool fullyTransparent = false;
    bool hasOwner = false;
    LONG_PTR style = 0;
    LONG_PTR extendedStyle = 0;
    RECT bounds{};
    std::wstring className;
    std::wstring processName;
};

struct WindowActivity {
    std::vector<RECT> applicationWindows;
    std::vector<RECT> transientWindows;
};

// Pure classification used by both the live enumerator and regression tests.
bool IsApplicationWindow(const WindowActivitySnapshot& window);
bool IsTaskbarTransientWindow(const WindowActivitySnapshot& window);

// Returns the screen rectangles of ordinary, visible, non-minimized app
// windows. Shell surfaces such as Start and Quick Settings are excluded.
std::vector<RECT> EnumerateVisibleApplicationWindows();
WindowActivity EnumerateWindowActivity();

bool SubstantiallyIntersects(const RECT& window, const RECT& monitor);
bool IsNearTaskbar(
    const RECT& window,
    const RECT& taskbar,
    const RECT& monitor,
    int maximumGap = 120);

}  // namespace chameleon
