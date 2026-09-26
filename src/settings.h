#pragma once

#include <string>

namespace chameleon {

enum class SamplingPreset {
    Exact = 0,
    Balanced = 1,
    Calm = 2,
    Custom = 3,
};

struct Settings {
    SamplingPreset preset = SamplingPreset::Exact;
    bool autoTransparency = false;
    bool showFirstRunHelp = true;
    bool launchHidden = false;
    bool adaptiveTrayContrast = true;
    bool backgroundOnly = true;
    bool transparentOnDesktop = true;
    bool freezeOnTaskbarActivity = true;
    int sampleDepth = 56;
    int sampleOffset = 0;
    int cellWidth = 4;
    int horizontalBlurPixels = 8;
    int blurStrengthPercent = 22;
    int intervalMilliseconds = 42;
    int transitionMilliseconds = 115;
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

RuntimeTuning TuningForPreset(SamplingPreset preset);
RuntimeTuning TuningForSettings(const Settings& settings);
void ApplyPreset(Settings& settings, SamplingPreset preset);
std::wstring PresetName(SamplingPreset preset);

std::wstring SettingsDirectory();
std::wstring SettingsPath();
Settings LoadSettings();
bool SaveSettings(const Settings& settings);

bool IsRunAtStartupEnabled();
bool SetRunAtStartupEnabled(bool enabled);

}  // namespace chameleon
