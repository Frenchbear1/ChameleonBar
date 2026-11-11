#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "surface_protocol.h"

namespace chameleon {

struct BridgeHealth {
    bool alive = false;      // the bridge pulsed recently
    int surfaces = 0;        // taskbars it is painting
    int trays = 0;
    int indicators = 0;
};

// Security attributes for the shared mapping and fail-safe event. They let the
// non-elevated Explorer reach both objects even when ChameleonBar itself was
// started as administrator (whose default DACL would lock Explorer out).
class SharedObjectSecurity {
public:
    SharedObjectSecurity();
    ~SharedObjectSecurity();
    SharedObjectSecurity(const SharedObjectSecurity&) = delete;
    SharedObjectSecurity& operator=(const SharedObjectSecurity&) = delete;

    // Null when the descriptor could not be built (defaults are used then).
    SECURITY_ATTRIBUTES* get() { return descriptor_ ? &attributes_ : nullptr; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

class SurfacePublisher {
public:
    SurfacePublisher() = default;
    ~SurfacePublisher();

    SurfacePublisher(const SurfacePublisher&) = delete;
    SurfacePublisher& operator=(const SurfacePublisher&) = delete;

    bool Initialize();

    // Publishes one pixel per taskbar pixel along its long axis.
    bool Publish(
        HWND taskbar,
        const RECT& taskbarRect,
        bool horizontal,
        const std::vector<std::uint32_t>& colors,
        SurfaceMode mode = kSurfaceImage);

    // Asks the bridge to restore the native taskbar for this window.
    void SetNative(HWND taskbar);

    // Frees the slot of a taskbar that no longer exists.
    void Release(HWND taskbar);

    void SetContrast(bool enabled, int lightPercent, int darkPercent);

    BridgeHealth Health() const;

private:
    SharedSurfaceSlot* FindSlot(HWND taskbar);
    SharedSurfaceSlot* FindOrCreateSlot(HWND taskbar);
    void BeginWrite();
    void EndWrite();

    HANDLE mapping_ = nullptr;
    SharedSurfaceState* state_ = nullptr;
};

}  // namespace chameleon
