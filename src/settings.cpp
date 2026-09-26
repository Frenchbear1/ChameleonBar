#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

namespace chameleon {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ChameleonBar";

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

int ReadInteger(const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, SettingsPath().c_str()));
}

}  // namespace

RuntimeTuning TuningForPreset(SamplingPreset preset) {
    switch (preset) {
    case SamplingPreset::Balanced:
        return {72, 0, 6, 16, 55, 190.0, 58.0f, 0.34f, 1.0f, 1.0f, true, 32};
    case SamplingPreset::Calm:
        return {112, 0, 10, 28, 80, 380.0, 64.0f, 0.45f, 1.0f, 1.0f, true, 40};
    case SamplingPreset::Custom:
    case SamplingPreset::Exact:
    default:
        return {56, 0, 4, 8, 42, 115.0, 50.0f, 0.22f, 1.0f, 1.0f, true, 24};
    }
}

RuntimeTuning TuningForSettings(const Settings& settings) {
    return {
        std::clamp(settings.sampleDepth, 1, 200),
        std::clamp(settings.sampleOffset, 0, 160),
        std::clamp(settings.cellWidth, 1, 24),
        std::clamp(settings.horizontalBlurPixels, 0, 200),
        std::clamp(settings.intervalMilliseconds, 0, 250),
        static_cast<double>(std::clamp(settings.transitionMilliseconds, 0, 1000)),
        static_cast<float>(std::clamp(settings.edgeThreshold, 10, 180)),
        std::clamp(settings.blurStrengthPercent, 0, 100) / 100.0f,
        std::clamp(settings.saturationPercent, 0, 200) / 100.0f,
        std::clamp(settings.brightnessPercent, 50, 150) / 100.0f,
        settings.backgroundOnly,
        std::clamp(settings.detailRejectionPixels, 0, 192),
    };
}

void ApplyPreset(Settings& settings, SamplingPreset preset) {
    if (preset == SamplingPreset::Custom) {
        settings.preset = preset;
        return;
    }
    const RuntimeTuning tuning = TuningForPreset(preset);
    settings.preset = preset;
    settings.sampleDepth = tuning.sampleDepth;
    settings.sampleOffset = tuning.sampleOffset;
    settings.cellWidth = tuning.cellWidth;
    settings.horizontalBlurPixels = tuning.horizontalBlurPixels;
    settings.blurStrengthPercent =
        static_cast<int>(std::lround(tuning.blurStrength * 100.0f));
    settings.intervalMilliseconds = tuning.intervalMilliseconds;
    settings.transitionMilliseconds =
        static_cast<int>(std::lround(tuning.transitionMilliseconds));
    settings.edgeThreshold = static_cast<int>(std::lround(tuning.edgeThreshold));
    settings.saturationPercent =
        static_cast<int>(std::lround(tuning.saturation * 100.0f));
    settings.brightnessPercent =
        static_cast<int>(std::lround(tuning.brightness * 100.0f));
    settings.backgroundOnly = tuning.backgroundOnly;
    settings.detailRejectionPixels = tuning.detailRejectionPixels;
}

std::wstring PresetName(SamplingPreset preset) {
    switch (preset) {
    case SamplingPreset::Balanced:
        return L"Balanced";
    case SamplingPreset::Calm:
        return L"Calm";
    case SamplingPreset::Custom:
        return L"Custom";
    case SamplingPreset::Exact:
    default:
        return L"Exact edges";
    }
}

std::wstring SettingsDirectory() {
    wchar_t path[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path))) {
        return L".";
    }
    return std::wstring(path) + L"\\ChameleonBar";
}

std::wstring SettingsPath() {
    return SettingsDirectory() + L"\\settings.ini";
}

Settings LoadSettings() {
    Settings settings;
    const int preset = ReadInteger(L"appearance", L"preset", static_cast<int>(settings.preset));
    settings.preset = static_cast<SamplingPreset>(std::max(0, std::min(3, preset)));
    settings.autoTransparency = ReadInteger(L"appearance", L"auto_transparency", 0) != 0;
    settings.showFirstRunHelp = ReadInteger(L"application", L"show_first_run_help", 1) != 0;
    settings.launchHidden = ReadInteger(L"application", L"launch_hidden", 0) != 0;
    settings.adaptiveTrayContrast = ReadInteger(L"contrast", L"enabled", 1) != 0;
    settings.backgroundOnly = ReadInteger(L"sampling", L"background_only", 1) != 0;
    settings.transparentOnDesktop =
        ReadInteger(L"appearance", L"transparent_on_desktop", 1) != 0;
    settings.freezeOnTaskbarActivity =
        ReadInteger(L"appearance", L"freeze_on_taskbar_activity", 1) != 0;
    const RuntimeTuning presetTuning = TuningForPreset(settings.preset);
    settings.sampleDepth = ReadInteger(L"sampling", L"depth", presetTuning.sampleDepth);
    settings.sampleOffset = ReadInteger(L"sampling", L"offset", presetTuning.sampleOffset);
    settings.cellWidth = ReadInteger(L"sampling", L"cell_width", presetTuning.cellWidth);
    settings.horizontalBlurPixels = ReadInteger(
        L"appearance", L"horizontal_blur", presetTuning.horizontalBlurPixels);
    settings.blurStrengthPercent = ReadInteger(
        L"appearance", L"blur_strength",
        static_cast<int>(std::lround(presetTuning.blurStrength * 100.0f)));
    settings.intervalMilliseconds = ReadInteger(
        L"sampling", L"interval_ms", presetTuning.intervalMilliseconds);
    settings.transitionMilliseconds = ReadInteger(
        L"appearance", L"transition_ms",
        static_cast<int>(std::lround(presetTuning.transitionMilliseconds)));
    settings.edgeThreshold = ReadInteger(
        L"sampling", L"edge_threshold",
        static_cast<int>(std::lround(presetTuning.edgeThreshold)));
    settings.saturationPercent = ReadInteger(L"appearance", L"saturation", 100);
    settings.brightnessPercent = ReadInteger(L"appearance", L"brightness", 100);
    settings.detailRejectionPixels = ReadInteger(
        L"sampling", L"detail_rejection_pixels",
        presetTuning.detailRejectionPixels);
    settings.lightGlyphThresholdPercent = ReadInteger(
        L"contrast", L"light_threshold", 18);
    settings.darkGlyphThresholdPercent = ReadInteger(
        L"contrast", L"dark_threshold", 26);

    const RuntimeTuning clamped = TuningForSettings(settings);
    settings.sampleDepth = clamped.sampleDepth;
    settings.sampleOffset = clamped.sampleOffset;
    settings.cellWidth = clamped.cellWidth;
    settings.horizontalBlurPixels = clamped.horizontalBlurPixels;
    settings.blurStrengthPercent =
        static_cast<int>(std::lround(clamped.blurStrength * 100.0f));
    settings.intervalMilliseconds = clamped.intervalMilliseconds;
    settings.transitionMilliseconds =
        static_cast<int>(std::lround(clamped.transitionMilliseconds));
    settings.edgeThreshold = static_cast<int>(std::lround(clamped.edgeThreshold));
    settings.saturationPercent =
        static_cast<int>(std::lround(clamped.saturation * 100.0f));
    settings.brightnessPercent =
        static_cast<int>(std::lround(clamped.brightness * 100.0f));
    settings.backgroundOnly = clamped.backgroundOnly;
    settings.detailRejectionPixels = clamped.detailRejectionPixels;
    settings.lightGlyphThresholdPercent =
        std::clamp(settings.lightGlyphThresholdPercent, 1, 90);
    settings.darkGlyphThresholdPercent =
        std::clamp(settings.darkGlyphThresholdPercent,
                   settings.lightGlyphThresholdPercent + 1, 95);
    return settings;
}

bool SaveSettings(const Settings& settings) {
    CreateDirectoryW(SettingsDirectory().c_str(), nullptr);
    const std::wstring path = SettingsPath();
    const std::wstring preset = std::to_wstring(static_cast<int>(settings.preset));
    const bool a = WritePrivateProfileStringW(L"appearance", L"preset", preset.c_str(), path.c_str()) != FALSE;
    const bool b = WritePrivateProfileStringW(
        L"appearance",
        L"auto_transparency",
        settings.autoTransparency ? L"1" : L"0",
        path.c_str()) != FALSE;
    const bool c = WritePrivateProfileStringW(
        L"application",
        L"show_first_run_help",
        settings.showFirstRunHelp ? L"1" : L"0",
        path.c_str()) != FALSE;
    const auto writeInteger = [&path](const wchar_t* section,
                                      const wchar_t* key,
                                      int value) {
        const std::wstring text = std::to_wstring(value);
        return WritePrivateProfileStringW(section, key, text.c_str(),
                                          path.c_str()) != FALSE;
    };
    const bool d = WritePrivateProfileStringW(
        L"application", L"launch_hidden",
        settings.launchHidden ? L"1" : L"0", path.c_str()) != FALSE;
    const bool e = WritePrivateProfileStringW(
        L"contrast", L"enabled",
        settings.adaptiveTrayContrast ? L"1" : L"0", path.c_str()) != FALSE;
    const bool f = WritePrivateProfileStringW(
        L"sampling", L"background_only",
        settings.backgroundOnly ? L"1" : L"0", path.c_str()) != FALSE;
    const bool g = WritePrivateProfileStringW(
        L"appearance", L"transparent_on_desktop",
        settings.transparentOnDesktop ? L"1" : L"0", path.c_str()) != FALSE;
    const bool h = WritePrivateProfileStringW(
        L"appearance", L"freeze_on_taskbar_activity",
        settings.freezeOnTaskbarActivity ? L"1" : L"0", path.c_str()) != FALSE;
    // Remove settings left behind by the retired experimental Animations page.
    WritePrivateProfileStringW(
        L"animations", nullptr, nullptr, path.c_str());
    const bool numbers =
        writeInteger(L"sampling", L"depth", settings.sampleDepth) &&
        writeInteger(L"sampling", L"offset", settings.sampleOffset) &&
        writeInteger(L"sampling", L"cell_width", settings.cellWidth) &&
        writeInteger(L"sampling", L"interval_ms", settings.intervalMilliseconds) &&
        writeInteger(L"sampling", L"edge_threshold", settings.edgeThreshold) &&
        writeInteger(L"sampling", L"detail_rejection_pixels", settings.detailRejectionPixels) &&
        writeInteger(L"appearance", L"horizontal_blur", settings.horizontalBlurPixels) &&
        writeInteger(L"appearance", L"blur_strength", settings.blurStrengthPercent) &&
        writeInteger(L"appearance", L"transition_ms", settings.transitionMilliseconds) &&
        writeInteger(L"appearance", L"saturation", settings.saturationPercent) &&
        writeInteger(L"appearance", L"brightness", settings.brightnessPercent) &&
        writeInteger(L"contrast", L"light_threshold", settings.lightGlyphThresholdPercent) &&
        writeInteger(L"contrast", L"dark_threshold", settings.darkGlyphThresholdPercent);
    return a && b && c && d && e && f && g && h && numbers;
}

bool IsRunAtStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[32768]{};
    DWORD size = sizeof(value);
    const LONG status = RegQueryValueExW(
        key,
        kRunValue,
        nullptr,
        nullptr,
        reinterpret_cast<BYTE*>(value),
        &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring expected = L"\"" + ExecutablePath() + L"\" --startup";
    return expected == value;
}

bool SetRunAtStartupEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRunKey,
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring value = L"\"" + ExecutablePath() + L"\" --startup";
        status = RegSetValueExW(
            key,
            kRunValue,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

}  // namespace chameleon
