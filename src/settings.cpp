#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <vector>

namespace chameleon {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ChameleonBar";

//                       depth off cell blur  int  trans  edge  blurS  sat   bright bgOnly detail
constexpr std::array<PresetInfo, 7> kPresets{{
    {SamplingPreset::Seamless, L"seamless", L"Seamless",
     L"Pixel-exact match. Snapped windows keep crisp edges.",
     {48, 0, 4, 8, 33, 120.0, 50.0f, 0.22f, 1.00f, 1.00f, true, 24}},
    {SamplingPreset::Soft, L"soft", L"Soft blend",
     L"Neighbouring colors melt together for a smoother bar.",
     {64, 0, 6, 56, 33, 220.0, 58.0f, 0.60f, 1.00f, 1.00f, true, 40}},
    {SamplingPreset::Ambient, L"ambient", L"Ambient glow",
     L"Wide, saturated light, like a bias-lit monitor.",
     {96, 0, 8, 160, 40, 420.0, 72.0f, 0.90f, 1.30f, 1.05f, true, 64}},
    {SamplingPreset::Cinema, L"cinema", L"Cinema",
     L"Slow, steady changes. No flicker during video or games.",
     {120, 0, 10, 40, 50, 650.0, 64.0f, 0.50f, 1.00f, 0.95f, true, 96}},
    {SamplingPreset::Vivid, L"vivid", L"Vivid",
     L"Punchier color pulled from the same pixels.",
     {56, 0, 4, 12, 33, 140.0, 50.0f, 0.30f, 1.45f, 1.05f, true, 24}},
    {SamplingPreset::Muted, L"muted", L"Muted",
     L"Desaturated and dimmed for a quieter desktop.",
     {64, 0, 6, 24, 40, 260.0, 56.0f, 0.40f, 0.55f, 0.85f, true, 32}},
    {SamplingPreset::Eco, L"eco", L"Battery saver",
     L"Samples less often in larger blocks to save power.",
     {40, 0, 12, 24, 125, 300.0, 58.0f, 0.40f, 1.00f, 1.00f, true, 36}},
}};

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

std::wstring ReadString(const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[128]{};
    GetPrivateProfileStringW(
        section, key, L"", buffer, static_cast<DWORD>(std::size(buffer)),
        SettingsPath().c_str());
    return buffer;
}

void CopyTuning(Settings& settings, const RuntimeTuning& tuning) {
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

SamplingPreset PresetFromKey(const std::wstring& key, bool& found) {
    found = true;
    if (key == L"custom") {
        return SamplingPreset::Custom;
    }
    for (const auto& preset : kPresets) {
        if (key == preset.key) {
            return preset.id;
        }
    }
    found = false;
    return SamplingPreset::Seamless;
}

}  // namespace

std::span<const PresetInfo> Presets() {
    return kPresets;
}

const PresetInfo* FindPreset(SamplingPreset preset) {
    for (const auto& info : kPresets) {
        if (info.id == preset) {
            return &info;
        }
    }
    return nullptr;
}

RuntimeTuning TuningForPreset(SamplingPreset preset) {
    const PresetInfo* info = FindPreset(preset);
    return info ? info->tuning : kPresets.front().tuning;
}

RuntimeTuning TuningForSettings(const Settings& settings) {
    return {
        std::clamp(settings.sampleDepth, 1, 200),
        std::clamp(settings.sampleOffset, 0, 160),
        std::clamp(settings.cellWidth, 1, 24),
        std::clamp(settings.horizontalBlurPixels, 0, 200),
        std::clamp(settings.intervalMilliseconds,
                   kMinimumIntervalMilliseconds,
                   kMaximumIntervalMilliseconds),
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
    settings.preset = preset;
    if (const PresetInfo* info = FindPreset(preset)) {
        CopyTuning(settings, info->tuning);
    }
}

SamplingPreset MatchingPreset(const Settings& settings) {
    for (const auto& preset : kPresets) {
        Settings candidate = settings;
        CopyTuning(candidate, preset.tuning);
        if (candidate.sampleDepth == settings.sampleDepth &&
            candidate.sampleOffset == settings.sampleOffset &&
            candidate.cellWidth == settings.cellWidth &&
            candidate.horizontalBlurPixels == settings.horizontalBlurPixels &&
            candidate.blurStrengthPercent == settings.blurStrengthPercent &&
            candidate.intervalMilliseconds == settings.intervalMilliseconds &&
            candidate.transitionMilliseconds == settings.transitionMilliseconds &&
            candidate.edgeThreshold == settings.edgeThreshold &&
            candidate.saturationPercent == settings.saturationPercent &&
            candidate.brightnessPercent == settings.brightnessPercent &&
            candidate.backgroundOnly == settings.backgroundOnly &&
            candidate.detailRejectionPixels == settings.detailRejectionPixels) {
            return preset.id;
        }
    }
    return SamplingPreset::Custom;
}

std::wstring PresetName(SamplingPreset preset) {
    if (const PresetInfo* info = FindPreset(preset)) {
        return info->name;
    }
    return L"Custom";
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

    // Preset: current releases store a stable key. Older releases stored an
    // index into a smaller list; map those onto the closest new preset and
    // adopt its tuned values rather than keeping stale ones.
    bool migratedFromLegacy = false;
    bool found = false;
    settings.preset = PresetFromKey(ReadString(L"appearance", L"preset_key"), found);
    if (!found) {
        const int legacy = ReadInteger(L"appearance", L"preset", -1);
        switch (legacy) {
        case 1: settings.preset = SamplingPreset::Soft; break;
        case 2: settings.preset = SamplingPreset::Cinema; break;
        case 3: settings.preset = SamplingPreset::Custom; break;
        default: settings.preset = SamplingPreset::Seamless; break;
        }
        migratedFromLegacy = legacy >= 0 && settings.preset != SamplingPreset::Custom;
    }

    settings.autoTransparency = ReadInteger(L"appearance", L"auto_transparency", 0) != 0;
    settings.launchHidden = ReadInteger(L"application", L"launch_hidden", 0) != 0;
    settings.adaptiveTrayContrast = ReadInteger(L"contrast", L"enabled", 1) != 0;
    settings.transparentOnDesktop =
        ReadInteger(L"appearance", L"transparent_on_desktop", 1) != 0;
    settings.freezeOnTaskbarActivity =
        ReadInteger(L"appearance", L"freeze_on_taskbar_activity", 1) != 0;
    settings.lightGlyphThresholdPercent = ReadInteger(
        L"contrast", L"light_threshold", 18);
    settings.darkGlyphThresholdPercent = ReadInteger(
        L"contrast", L"dark_threshold", 26);

    if (migratedFromLegacy) {
        ApplyPreset(settings, settings.preset);
    } else {
        Settings defaults;
        ApplyPreset(defaults, settings.preset == SamplingPreset::Custom
                                  ? SamplingPreset::Seamless
                                  : settings.preset);
        settings.backgroundOnly =
            ReadInteger(L"sampling", L"background_only", defaults.backgroundOnly) != 0;
        settings.sampleDepth = ReadInteger(L"sampling", L"depth", defaults.sampleDepth);
        settings.sampleOffset = ReadInteger(L"sampling", L"offset", defaults.sampleOffset);
        settings.cellWidth = ReadInteger(L"sampling", L"cell_width", defaults.cellWidth);
        settings.intervalMilliseconds = ReadInteger(
            L"sampling", L"interval_ms", defaults.intervalMilliseconds);
        settings.edgeThreshold = ReadInteger(
            L"sampling", L"edge_threshold", defaults.edgeThreshold);
        settings.detailRejectionPixels = ReadInteger(
            L"sampling", L"detail_rejection_pixels", defaults.detailRejectionPixels);
        settings.horizontalBlurPixels = ReadInteger(
            L"appearance", L"horizontal_blur", defaults.horizontalBlurPixels);
        settings.blurStrengthPercent = ReadInteger(
            L"appearance", L"blur_strength", defaults.blurStrengthPercent);
        settings.transitionMilliseconds = ReadInteger(
            L"appearance", L"transition_ms", defaults.transitionMilliseconds);
        settings.saturationPercent = ReadInteger(
            L"appearance", L"saturation", defaults.saturationPercent);
        settings.brightnessPercent = ReadInteger(
            L"appearance", L"brightness", defaults.brightnessPercent);
    }

    // Clamp everything into range (this also turns the retired "Instant"
    // interval of 0 ms into the fastest supported rate).
    const RuntimeTuning clamped = TuningForSettings(settings);
    const SamplingPreset preset = settings.preset;
    CopyTuning(settings, clamped);
    settings.preset = preset;
    settings.lightGlyphThresholdPercent =
        std::clamp(settings.lightGlyphThresholdPercent, 1, 90);
    settings.darkGlyphThresholdPercent =
        std::clamp(settings.darkGlyphThresholdPercent,
                   settings.lightGlyphThresholdPercent + 1, 95);
    if (settings.preset != SamplingPreset::Custom &&
        MatchingPreset(settings) != settings.preset) {
        settings.preset = SamplingPreset::Custom;
    }
    return settings;
}

bool SaveSettings(const Settings& settings) {
    CreateDirectoryW(SettingsDirectory().c_str(), nullptr);
    const std::wstring path = SettingsPath();
    bool ok = true;
    const auto writeString = [&](const wchar_t* section, const wchar_t* key,
                                 const wchar_t* value) {
        ok = WritePrivateProfileStringW(section, key, value, path.c_str()) != FALSE && ok;
    };
    const auto writeBool = [&](const wchar_t* section, const wchar_t* key, bool value) {
        writeString(section, key, value ? L"1" : L"0");
    };
    const auto writeInteger = [&](const wchar_t* section, const wchar_t* key, int value) {
        const std::wstring text = std::to_wstring(value);
        writeString(section, key, text.c_str());
    };

    const PresetInfo* info = FindPreset(settings.preset);
    writeString(L"appearance", L"preset_key", info ? info->key : L"custom");
    // Retired keys from earlier previews.
    WritePrivateProfileStringW(L"appearance", L"preset", nullptr, path.c_str());
    WritePrivateProfileStringW(L"application", L"show_first_run_help", nullptr, path.c_str());
    WritePrivateProfileStringW(L"animations", nullptr, nullptr, path.c_str());

    writeBool(L"appearance", L"auto_transparency", settings.autoTransparency);
    writeBool(L"application", L"launch_hidden", settings.launchHidden);
    writeBool(L"contrast", L"enabled", settings.adaptiveTrayContrast);
    writeBool(L"sampling", L"background_only", settings.backgroundOnly);
    writeBool(L"appearance", L"transparent_on_desktop", settings.transparentOnDesktop);
    writeBool(L"appearance", L"freeze_on_taskbar_activity", settings.freezeOnTaskbarActivity);
    writeInteger(L"sampling", L"depth", settings.sampleDepth);
    writeInteger(L"sampling", L"offset", settings.sampleOffset);
    writeInteger(L"sampling", L"cell_width", settings.cellWidth);
    writeInteger(L"sampling", L"interval_ms", settings.intervalMilliseconds);
    writeInteger(L"sampling", L"edge_threshold", settings.edgeThreshold);
    writeInteger(L"sampling", L"detail_rejection_pixels", settings.detailRejectionPixels);
    writeInteger(L"appearance", L"horizontal_blur", settings.horizontalBlurPixels);
    writeInteger(L"appearance", L"blur_strength", settings.blurStrengthPercent);
    writeInteger(L"appearance", L"transition_ms", settings.transitionMilliseconds);
    writeInteger(L"appearance", L"saturation", settings.saturationPercent);
    writeInteger(L"appearance", L"brightness", settings.brightnessPercent);
    writeInteger(L"contrast", L"light_threshold", settings.lightGlyphThresholdPercent);
    writeInteger(L"contrast", L"dark_threshold", settings.darkGlyphThresholdPercent);
    return ok;
}

bool IsRunAtStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[32768]{};
    DWORD size = sizeof(value) - sizeof(wchar_t);
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
    return _wcsicmp(expected.c_str(), value) == 0;
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
