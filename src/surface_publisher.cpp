#include "surface_publisher.h"

#include <algorithm>
#include <cstring>

namespace chameleon {

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
        std::memset(state_, 0, sizeof(*state_));
        state_->version = kSurfaceProtocolVersion;
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

SharedSurfaceSlot* SurfacePublisher::FindOrCreateSlot(HWND taskbar) {
    if (!state_ || !taskbar) {
        return nullptr;
    }
    const auto handle = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(taskbar));
    SharedSurfaceSlot* empty = nullptr;
    for (auto& slot : state_->slots) {
        if (slot.taskbarWindow == handle) {
            return &slot;
        }
        if (!empty && slot.taskbarWindow == 0) {
            empty = &slot;
        }
    }
    if (empty) {
        empty->taskbarWindow = handle;
    }
    return empty;
}

bool SurfacePublisher::Publish(
    HWND taskbar,
    const RECT& taskbarRect,
    bool horizontal,
    const std::vector<std::uint32_t>& colors) {
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
    slot->visible = 1;
    ++slot->revision;
    EndWrite();
    return true;
}

void SurfacePublisher::Hide(HWND taskbar) {
    if ((!state_ && !Initialize()) || !taskbar) {
        return;
    }
    BeginWrite();
    if (SharedSurfaceSlot* slot = FindOrCreateSlot(taskbar)) {
        slot->visible = 0;
        ++slot->revision;
    }
    EndWrite();
}

}  // namespace chameleon
