#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "surface_protocol.h"

namespace chameleon {

class SurfacePublisher {
public:
    SurfacePublisher() = default;
    ~SurfacePublisher();

    SurfacePublisher(const SurfacePublisher&) = delete;
    SurfacePublisher& operator=(const SurfacePublisher&) = delete;

    bool Initialize();
    bool Publish(
        HWND taskbar,
        const RECT& taskbarRect,
        bool horizontal,
        const std::vector<std::uint32_t>& colors);
    void Hide(HWND taskbar);

private:
    SharedSurfaceSlot* FindOrCreateSlot(HWND taskbar);
    void BeginWrite();
    void EndWrite();

    HANDLE mapping_ = nullptr;
    SharedSurfaceState* state_ = nullptr;
};

}  // namespace chameleon
