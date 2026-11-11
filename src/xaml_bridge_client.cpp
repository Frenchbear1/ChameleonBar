#include "xaml_bridge_client.h"

#include <windows.h>

#include <array>
#include <string>

namespace chameleon {
namespace {

// Declared here so the app does not depend on xamlom.h being present.
using InitializeXamlDiagnosticsExFunction = HRESULT(WINAPI*)(
    LPCWSTR endPointName,
    DWORD pid,
    LPCWSTR xamlDiagnosticsDll,
    LPCWSTR tapDll,
    CLSID tapClsid,
    LPCWSTR initializationData);

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
            result.back() == L' ' || result.back() == L'.')) {
        result.pop_back();
    }
    return result;
}

bool IsBridgeReady() {
    HANDLE readyEvent = OpenEventW(SYNCHRONIZE, FALSE, kBridgeReadyEventName);
    if (!readyEvent) {
        return false;
    }
    const bool ready = WaitForSingleObject(readyEvent, 0) == WAIT_OBJECT_0;
    CloseHandle(readyEvent);
    return ready;
}

std::wstring WindowsDirectoryPath() {
    wchar_t buffer[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
    return length && length < MAX_PATH ? std::wstring(buffer, length) : L"C:\\Windows";
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
    if (IsBridgeReady()) {
        result.status = S_OK;
        result.attached = true;
        result.message = L"Bridge already loaded in Explorer";
        return result;
    }

    const std::wstring bridgePath = LocalBridgePath();
    if (bridgePath.empty() ||
        GetFileAttributesW(bridgePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.status = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        result.message =
            L"ChameleonBar.XamlBridge.dll is missing. Keep it next to ChameleonBar.exe";
        return result;
    }

    const DWORD explorerProcessId = ExplorerTaskbarProcessId();
    if (!explorerProcessId) {
        result.status = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        result.retryable = true;
        result.message = L"Waiting for the Windows taskbar to start";
        return result;
    }

    HMODULE xaml = LoadLibraryExW(
        L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) {
        result.status = HRESULT_FROM_WIN32(GetLastError());
        result.message = L"The Windows XAML runtime is unavailable";
        return result;
    }
    const auto initialize =
        reinterpret_cast<InitializeXamlDiagnosticsExFunction>(
            reinterpret_cast<void*>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")));
    if (!initialize) {
        result.status = HRESULT_FROM_WIN32(GetLastError());
        result.message = L"This Windows build does not support the taskbar bridge";
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
            kChameleonBarTapClsid,
            nullptr);
        if (status != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }
    FreeLibrary(xaml);

    result.status = status;
    result.attached = SUCCEEDED(status);
    if (result.attached) {
        result.message = L"Bridge loaded into Explorer";
    } else if (status == CLASS_E_CLASSNOTAVAILABLE) {
        result.message =
            L"An older ChameleonBar bridge is still loaded. Restart Explorer once";
    } else {
        result.retryable = true;
        result.message = L"Bridge could not attach: " + StatusText(status);
    }
    return result;
}

bool RestartExplorer() {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar) {
        DWORD processId = 0;
        GetWindowThreadProcessId(taskbar, &processId);
        HANDLE process = OpenProcess(
            SYNCHRONIZE | PROCESS_TERMINATE, FALSE, processId);
        // 0x5B4 is the message behind "Exit Explorer" on the taskbar context
        // menu: a clean shutdown that saves state and does not auto-restart.
        PostMessageW(taskbar, WM_USER + 436, 0, 0);
        if (process) {
            if (WaitForSingleObject(process, 8000) != WAIT_OBJECT_0) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 3000);
            }
            CloseHandle(process);
        } else {
            Sleep(3000);
        }
        // Windows may restart a terminated shell by itself.
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (FindWindowW(L"Shell_TrayWnd", nullptr)) {
                return true;
            }
            Sleep(200);
        }
    }

    const std::wstring explorer = WindowsDirectoryPath() + L"\\explorer.exe";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    std::wstring commandLine = L"\"" + explorer + L"\"";
    if (!CreateProcessW(
            explorer.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &information)) {
        return false;
    }
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
    return true;
}

}  // namespace chameleon
