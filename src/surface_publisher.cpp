#include "surface_publisher.h"

#include <algorithm>
#include <cstring>

namespace chameleon {
namespace {

std::uint64_t HandleKey(HWND window) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window));
}

}  // namespace

SurfacePublisher::~SurfacePublisher() {
    if (state_) {
        UnmapViewOfFile(state_);
    }
    if (mapping_) {
        CloseHandle(mapping_);
    }
}

bool SurfacePublisher::Initialize() {
    if (state_) {
        return true;
    }
    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(SharedSurfaceState)),
        kSurfaceMappingName);
    if (!mapping_) {
        return false;
    }
    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    state_ = static_cast<SharedSurfaceState*>(MapViewOfFile(
        mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedSurfaceState)));
    if (!state_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    if (created || state_->version != kSurfaceProtocolVersion) {
        std::memset(static_cast<void*>(state_), 0, sizeof(*state_));
        state_->version = kSurfaceProtocolVersion;
        state_->contrastEnabled = 1;
        state_->lightThresholdPercent = 18;
        state_->darkThresholdPercent = 26;
    } else {
        // The mapping outlived a previous ChameleonBar instance because the
        // bridge still holds it. A crash mid-write can leave the seqlock odd;
        // even it out, then forget taskbars that no longer exist.
        if ((state_->sequence & 1) != 0) {
            InterlockedIncrement(&state_->sequence);
        }
        BeginWrite();
        for (auto& slot : state_->slots) {
            const HWND window = reinterpret_cast<HWND>(
                static_cast<std::uintptr_t>(slot.taskbarWindow));
            if (slot.taskbarWindow != 0 && !IsWindow(window)) {
                slot.taskbarWindow = 0;
                slot.mode = kSurfaceNative;
                slot.pixelCount = 0;
                ++slot.revision;
            }
        }
        EndWrite();
    }
    return true;
}

void SurfacePublisher::BeginWrite() {
    InterlockedIncrement(&state_->sequence);
    MemoryBarrier();
}

void SurfacePublisher::EndWrite() {
    MemoryBarrier();
    InterlockedIncrement(&state_->sequence);
}

SharedSurfaceSlot* SurfacePublisher::FindSlot(HWND taskbar) {
    if (!state_ || !taskbar) {
        return nullptr;
    }
    const auto key = HandleKey(taskbar);
    for (auto& slot : state_->slots) {
        if (slot.taskbarWindow == key) {
            return &slot;
        }
    }
    return nullptr;
}

SharedSurfaceSlot* SurfacePublisher::FindOrCreateSlot(HWND taskbar) {
    if (SharedSurfaceSlot* existing = FindSlot(taskbar)) {
        return existing;
    }
    if (!state_ || !taskbar) {
        return nullptr;
    }
    SharedSurfaceSlot* empty = nullptr;
    for (auto& slot : state_->slots) {
        if (slot.taskbarWindow == 0) {
            empty = &slot;
            break;
        }
    }
    if (!empty) {
        // Every slot is taken. Reclaim one whose taskbar has been destroyed
        // (Explorer restarts and monitor changes create new taskbar windows).
        for (auto& slot : state_->slots) {
            const HWND window = reinterpret_cast<HWND>(
                static_cast<std::uintptr_t>(slot.taskbarWindow));
            if (!IsWindow(window)) {
                empty = &slot;
                break;
            }
        }
    }
    if (empty) {
        empty->taskbarWindow = HandleKey(taskbar);
        empty->mode = kSurfaceNative;
        empty->pixelCount = 0;
    }
    return empty;
}

bool SurfacePublisher::Publish(
    HWND taskbar,
    const RECT& taskbarRect,
    bool horizontal,
    const std::vector<std::uint32_t>& colors,
    SurfaceMode mode) {
    if ((!state_ && !Initialize()) || colors.empty()) {
        return false;
    }
    BeginWrite();
    SharedSurfaceSlot* slot = FindOrCreateSlot(taskbar);
    if (!slot) {
        EndWrite();
        return false;
    }
    const std::size_t count = std::min(
        colors.size(), static_cast<std::size_t>(kMaximumSurfacePixels));
    slot->left = taskbarRect.left;
    slot->top = taskbarRect.top;
    slot->right = taskbarRect.right;
    slot->bottom = taskbarRect.bottom;
    slot->pixelWidth = horizontal ? static_cast<LONG>(count) : 1;
    slot->pixelHeight = horizontal ? 1 : static_cast<LONG>(count);
    slot->pixelCount = static_cast<LONG>(count);
    std::copy_n(colors.data(), count, slot->pixels);
    slot->mode = mode;
    ++slot->revision;
    EndWrite();
    return true;
}

void SurfacePublisher::SetNative(HWND taskbar) {
    if (!state_ || !taskbar) {
        return;
    }
    BeginWrite();
    if (SharedSurfaceSlot* slot = FindSlot(taskbar)) {
        if (slot->mode != kSurfaceNative) {
            slot->mode = kSurfaceNative;
            ++slot->revision;
        }
    }
    EndWrite();
}

void SurfacePublisher::Release(HWND taskbar) {
    if (!state_ || !taskbar) {
        return;
    }
    BeginWrite();
    if (SharedSurfaceSlot* slot = FindSlot(taskbar)) {
        slot->taskbarWindow = 0;
        slot->mode = kSurfaceNative;
        slot->pixelCount = 0;
        ++slot->revision;
    }
    EndWrite();
}

void SurfacePublisher::SetContrast(
    bool enabled, int lightPercent, int darkPercent) {
    if (!state_ && !Initialize()) {
        return;
    }
    lightPercent = std::clamp(lightPercent, 1, 90);
    darkPercent = std::clamp(darkPercent, lightPercent + 1, 95);
    BeginWrite();
    state_->contrastEnabled = enabled ? 1 : 0;
    state_->lightThresholdPercent = lightPercent;
    state_->darkThresholdPercent = darkPercent;
    EndWrite();
}

BridgeHealth SurfacePublisher::Health() const {
    BridgeHealth health;
    if (!state_) {
        return health;
    }
    const LONG heartbeat = state_->bridgeHeartbeat;
    const DWORD age = GetTickCount() - static_cast<DWORD>(heartbeat);
    health.alive = heartbeat != 0 && age < 2500;
    if (health.alive) {
        health.surfaces = state_->bridgeSurfaces;
        health.trays = state_->bridgeTrays;
        health.indicators = state_->bridgeIndicators;
    }
    return health;
}

}  // namespace chameleon
