#pragma once

#include <windows.h>

#include <cstdint>

namespace chameleon {

constexpr wchar_t kSurfaceMappingName[] =
    L"Local\\ChameleonBar.Surface.v2";
constexpr std::uint32_t kSurfaceProtocolVersion = 2;
constexpr std::size_t kMaximumTaskbars = 8;
constexpr std::size_t kMaximumSurfacePixels = 8192;

struct SharedSurfaceSlot {
    std::uint64_t taskbarWindow = 0;
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
    LONG pixelWidth = 0;
    LONG pixelHeight = 0;
    LONG pixelCount = 0;
    LONG visible = 0;
    LONG revision = 0;
    std::uint32_t pixels[kMaximumSurfacePixels]{};
};

struct SharedSurfaceState {
    volatile LONG sequence = 0;
    std::uint32_t version = kSurfaceProtocolVersion;
    SharedSurfaceSlot slots[kMaximumTaskbars]{};
};

}  // namespace chameleon
