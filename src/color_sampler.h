#pragma once

#include <cstdint>
#include <vector>

namespace chameleon {

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

enum class AdjacentEdge {
    Top,
    Bottom,
    Left,
    Right,
};

struct SamplerConfig {
    int cellWidth = 4;
    int colorBinsPerChannel = 16;
    float edgeThreshold = 54.0f;
    int horizontalBlurPixels = 8;
    float blurStrength = 0.28f;
    bool backgroundOnly = true;
    int detailRejectionPixels = 24;
    // Pixel offsets along the sampled band where filtering must stop. The app
    // supplies visible application edges so a wide foreground-detail radius
    // can clean one window without borrowing color from a neighbor or desktop
    // gap.
    std::vector<int> protectedBoundaryPixels;
};

// Converts a captured BGRA screen band into a one-dimensional field of colors.
// The field runs parallel to the taskbar. Color selection uses a weighted mode;
// background-only sampling reduces the near-edge bias and rejects narrow
// horizontal detail so text and small icons do not make the taskbar shimmer.
std::vector<Rgb> ExtractColorField(
    const std::uint8_t* bgra,
    int width,
    int height,
    int strideBytes,
    AdjacentEdge edge,
    const SamplerConfig& config);

// Expands cell colors to one output color per taskbar pixel. Similar cells blend
// smoothly; large changes stay sharp so snapped-window boundaries are preserved.
std::vector<std::uint32_t> ExpandColorField(
    const std::vector<Rgb>& cells,
    int outputLength,
    int cellWidth,
    float edgeThreshold,
    bool interpolate = true);

void AdjustColorField(
    std::vector<Rgb>& colors,
    float saturation,
    float brightness);

float ColorDistance(const Rgb& a, const Rgb& b);

class TemporalFilter {
public:
    const std::vector<Rgb>& Update(
        const std::vector<Rgb>& target,
        double elapsedMilliseconds,
        double transitionMilliseconds);

    void Reset();

private:
    std::vector<Rgb> current_;
};

}  // namespace chameleon
