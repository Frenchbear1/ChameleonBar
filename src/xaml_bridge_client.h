#pragma once

#include <windows.h>

#include <string>

#include "surface_protocol.h"

namespace chameleon {

struct BridgeAttachResult {
    HRESULT status = E_FAIL;
    bool attached = false;
    // True when retrying later may succeed (Explorer still starting, etc.).
    bool retryable = false;
    std::wstring message;
};

std::wstring LocalBridgePath();
BridgeAttachResult AttachLocalXamlBridge();

// Gracefully exits Explorer and starts it again. Blocks for a few seconds, so
// call it from a worker thread. Returns false if Explorer could not be
// restarted.
bool RestartExplorer();

}  // namespace chameleon
