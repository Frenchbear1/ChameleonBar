#define WIN32_LEAN_AND_MEAN

#include "xaml_bridge_client.h"

#include <windows.h>
#include <xamlom.h>

#include <array>
#include <string>

namespace chameleon {
namespace {

// {7B5E11EE-9EC1-4BA2-AD74-2872EAF766FB}
constexpr CLSID kChameleonBarTap = {
    0x7b5e11ee,
    0x9ec1,
    0x4ba2,
    {0xad, 0x74, 0x28, 0x72, 0xea, 0xf7, 0x66, 0xfb}};

using InitializeXamlDiagnosticsExFunction = decltype(&InitializeXamlDiagnosticsEx);

DWORD ExplorerTaskbarProcessId() {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD processId = 0;
    if (taskbar) {
        GetWindowThreadProcessId(taskbar, &processId);
    }
    return processId;
}

std::wstring StatusText(HRESULT status) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(status),
        0,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::wstring result = length && buffer
        ? std::wstring(buffer, length)
        : L"Unknown XAML bridge error";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' ||
            result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

}  // namespace

std::wstring LocalBridgePath() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) {
        return {};
    }
    std::wstring path(executable.data(), length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"ChameleonBar.XamlBridge.dll";
    }
    path.resize(slash + 1);
    path += L"ChameleonBar.XamlBridge.dll";
    return path;
}

BridgeAttachResult AttachLocalXamlBridge() {
    BridgeAttachResult result;
    HANDLE readyEvent =
        OpenEventW(SYNCHRONIZE, FALSE, kBridgeReadyEventName);
    if (readyEvent) {
        const bool ready =
            WaitForSingleObject(readyEvent, 0) == WAIT_OBJECT_0;
        CloseHandle(readyEvent);
        if (ready) {
            result.status = S_OK;
            result.attached = true;
            result.message =
                L"Local XAML bridge already active — Windhawk is not required";
            return result;
        }
    }

    const std::wstring bridgePath = LocalBridgePath();
    if (bridgePath.empty() ||
        GetFileAttributesW(bridgePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.status = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        result.message = L"Local tray bridge DLL is missing";
        return result;
    }

    const DWORD explorerProcessId = ExplorerTaskbarProcessId();
    if (!explorerProcessId) {
        result.status = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        result.message = L"Windows taskbar process was not found";
        return result;
    }

    HMODULE xaml = LoadLibraryExW(
        L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) {
        result.status = HRESULT_FROM_WIN32(GetLastError());
        result.message = L"Windows XAML runtime is unavailable";
        return result;
    }
    const auto initialize =
        reinterpret_cast<InitializeXamlDiagnosticsExFunction>(
            GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"));
    if (!initialize) {
        result.status = HRESULT_FROM_WIN32(GetLastError());
        result.message = L"This Windows build does not expose the XAML diagnostics bridge";
        FreeLibrary(xaml);
        return result;
    }

    HRESULT status = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    for (int index = 1; index <= 10000; ++index) {
        wchar_t connectionName[64]{};
        swprintf_s(connectionName, L"VisualDiagConnection%d", index);
        status = initialize(
            connectionName,
            explorerProcessId,
            L"",
            bridgePath.c_str(),
            kChameleonBarTap,
            nullptr);
        if (status != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }
    FreeLibrary(xaml);

    result.status = status;
    result.attached = SUCCEEDED(status);
    result.message = result.attached
        ? L"Local XAML bridge active — Windhawk is not required"
        : L"Local XAML bridge unavailable: " + StatusText(status);
    return result;
}

}  // namespace chameleon
