#pragma once

#include <span>
#include <string>

namespace chameleon {

enum class SamplingPreset {
    Seamless = 0,
    Soft = 1,
    Ambient = 2,
    Cinema = 3,
    Vivid = 4,
    Muted = 5,
    Eco = 6,
    Custom = 7,
};

struct Settings {
    SamplingPreset preset = SamplingPreset::Seamless;
    bool autoTransparency = false;
    bool launchHidden = false;
    bool adaptiveTrayContrast = true;
    bool backgroundOnly = true;
    bool transparentOnDesktop = true;
    bool freezeOnTaskbarActivity = true;
    int sampleDepth = 48;
    int sampleOffset = 0;
    int cellWidth = 4;
    int horizontalBlurPixels = 8;
    int blurStrengthPercent = 22;
    int intervalMilliseconds = 33;
    int transitionMilliseconds = 120;
    int edgeThreshold = 50;
    int saturationPercent = 100;
    int brightnessPercent = 100;
    int detailRejectionPixels = 24;
    int lightGlyphThresholdPercent = 18;
    int darkGlyphThresholdPercent = 26;
};

struct RuntimeTuning {
    int sampleDepth;
    int sampleOffset;
    int cellWidth;
    int horizontalBlurPixels;
    int intervalMilliseconds;
    double transitionMilliseconds;
    float edgeThreshold;
    float blurStrength;
    float saturation;
    float brightness;
    bool backgroundOnly;
    int detailRejectionPixels;
};

struct PresetInfo {
    SamplingPreset id;
    const wchar_t* key;
    const wchar_t* name;
    const wchar_t* description;
    RuntimeTuning tuning;
};

// Valid ranges, shared by the settings loader and the UI.
constexpr int kMinimumIntervalMilliseconds = 16;
constexpr int kMaximumIntervalMilliseconds = 250;

std::span<const PresetInfo> Presets();
const PresetInfo* FindPreset(SamplingPreset preset);

RuntimeTuning TuningForPreset(SamplingPreset preset);
RuntimeTuning TuningForSettings(const Settings& settings);
void ApplyPreset(Settings& settings, SamplingPreset preset);
// Returns the preset whose values the settings exactly match, else Custom.
SamplingPreset MatchingPreset(const Settings& settings);
std::wstring PresetName(SamplingPreset preset);

std::wstring SettingsDirectory();
std::wstring SettingsPath();
Settings LoadSettings();
bool SaveSettings(const Settings& settings);

bool IsRunAtStartupEnabled();
bool SetRunAtStartupEnabled(bool enabled);

}  // namespace chameleon
