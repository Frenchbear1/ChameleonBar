#pragma once

#include <windows.h>

#include <cstdint>

namespace chameleon {

// Shared between ChameleonBar.exe (writer) and the XAML bridge loaded into
// Explorer (reader). Every name is versioned so a dormant bridge from an older
// release can stay loaded in Explorer without interfering with this one.
constexpr wchar_t kSurfaceMappingName[] =
    L"Local\\ChameleonBar.Surface.v3";
constexpr wchar_t kActiveEventName[] = L"Local\\ChameleonBar.Active.v5";
constexpr wchar_t kBridgeReadyEventName[] =
    L"Local\\ChameleonBar.XamlBridgeReady.v5";
constexpr std::uint32_t kSurfaceProtocolVersion = 3;
constexpr std::size_t kMaximumTaskbars = 8;
constexpr std::size_t kMaximumSurfacePixels = 8192;

// {3F0C7A52-6B1D-4E8A-9C3B-2D5E7F4A1B60}
constexpr CLSID kChameleonBarTapClsid = {
    0x3f0c7a52,
    0x6b1d,
    0x4e8a,
    {0x9c, 0x3b, 0x2d, 0x5e, 0x7f, 0x4a, 0x1b, 0x60}};

// What the bridge should draw for one taskbar.
enum SurfaceMode : LONG {
    // Leave the native Windows taskbar untouched.
    kSurfaceNative = 0,
    // Draw the published pixel strip.
    kSurfaceImage = 1,
    // Make the taskbar fully transparent. Pixels still describe what is
    // behind it so tray contrast can follow the wallpaper.
    kSurfaceClear = 2,
};

struct SharedSurfaceSlot {
    std::uint64_t taskbarWindow = 0;
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
    LONG pixelWidth = 0;
    LONG pixelHeight = 0;
    LONG pixelCount = 0;
    LONG mode = kSurfaceNative;
    LONG revision = 0;
    LONG reserved[3]{};
    std::uint32_t pixels[kMaximumSurfacePixels]{};
};

struct SharedSurfaceState {
    // Seqlock: odd while the app is writing.
    volatile LONG sequence = 0;
    std::uint32_t version = kSurfaceProtocolVersion;

    // Written by the app.
    LONG contrastEnabled = 1;
    LONG lightThresholdPercent = 18;
    LONG darkThresholdPercent = 26;
    LONG reserved[5]{};

    // Written by the bridge so the app can show a truthful status.
    volatile LONG bridgeHeartbeat = 0;  // GetTickCount() of the last pulse
    volatile LONG bridgeSurfaces = 0;   // taskbars currently painted
    volatile LONG bridgeTrays = 0;      // system trays under contrast control
    volatile LONG bridgeIndicators = 0; // running-app underlines tracked
    LONG bridgeReserved[4]{};

    SharedSurfaceSlot slots[kMaximumTaskbars]{};
};

}  // namespace chameleon
