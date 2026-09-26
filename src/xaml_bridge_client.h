#pragma once

#include <windows.h>

#include <string>

namespace chameleon {

constexpr wchar_t kActiveEventName[] = L"Local\\ChameleonBar.Active.v4";
constexpr wchar_t kBridgeReadyEventName[] =
    L"Local\\ChameleonBar.XamlBridgeReady.v4";

struct BridgeAttachResult {
    HRESULT status = E_FAIL;
    bool attached = false;
    std::wstring message;
};

std::wstring LocalBridgePath();
BridgeAttachResult AttachLocalXamlBridge();

}  // namespace chameleon
