#include "color_sampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace chameleon {
namespace {

struct Bucket {
    float weight = 0.0f;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
};

float ClampByte(float value) {
    return std::max(0.0f, std::min(255.0f, value));
}

Rgb Mix(const Rgb& a, const Rgb& b, float amount) {
    amount = std::max(0.0f, std::min(1.0f, amount));
    return {
        a.r + (b.r - a.r) * amount,
        a.g + (b.g - a.g) * amount,
        a.b + (b.b - a.b) * amount,
    };
}

int LongAxisLength(int width, int height, AdjacentEdge edge) {
    return (edge == AdjacentEdge::Top || edge == AdjacentEdge::Bottom)
        ? width
        : height;
}

int DepthLength(int width, int height, AdjacentEdge edge) {
    return (edge == AdjacentEdge::Top || edge == AdjacentEdge::Bottom)
        ? height
        : width;
}

void CoordinatesFor(
    int along,
    int depth,
    int width,
    int height,
    AdjacentEdge edge,
    int& x,
    int& y) {
    switch (edge) {
    case AdjacentEdge::Top:
        x = along;
        y = depth;
        break;
    case AdjacentEdge::Bottom:
        x = along;
        y = height - 1 - depth;
        break;
    case AdjacentEdge::Left:
        x = depth;
        y = along;
        break;
    case AdjacentEdge::Right:
        x = width - 1 - depth;
        y = along;
        break;
    }
}

std::vector<std::size_t> ProtectedCellBoundaries(
    const std::vector<int>& pixelBoundaries,
    int cellWidth,
    std::size_t cellCount) {
    std::vector<std::size_t> boundaries{0, cellCount};
    if (cellCount < 2) {
        return boundaries;
    }
    for (const int pixel : pixelBoundaries) {
        const int roundedCell =
            (std::max(0, pixel) + std::max(1, cellWidth) / 2) /
            std::max(1, cellWidth);
        if (roundedCell > 0 &&
            roundedCell < static_cast<int>(cellCount)) {
            boundaries.push_back(static_cast<std::size_t>(roundedCell));
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(
        std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

void RemoveNarrowForegroundRuns(
    std::vector<Rgb>& colors,
    int cellWidth,
    int detailRejectionPixels,
    float edgeThreshold,
    const std::vector<int>& protectedBoundaryPixels) {
    if (colors.size() < 3 || detailRejectionPixels <= 0) {
        return;
    }

    const int radiusCells = std::max(
        1,
        static_cast<int>(std::ceil(
            static_cast<float>(detailRejectionPixels) /
            std::max(1, cellWidth))));
    const float foregroundThreshold =
        std::max(24.0f, edgeThreshold * 0.55f);

    // A one-dimensional median background estimate removes arbitrary glyph and
    // icon shapes, including adjacent antialiased strokes that do not form one
    // clean color run. A symmetric median keeps a genuine step edge at the same
    // coordinate, so split-screen boundaries do not bleed or shift.
    const std::vector<Rgb> source = colors;
    const std::size_t maximumWindow = static_cast<std::size_t>(
        radiusCells * 2 + 1);
    std::vector<float> red;
    std::vector<float> green;
    std::vector<float> blue;
    red.reserve(maximumWindow);
    green.reserve(maximumWindow);
    blue.reserve(maximumWindow);

    const auto median = [](std::vector<float>& values) {
        const auto middle = values.begin() +
            static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), middle, values.end());
        return *middle;
    };

    const auto medianColor = [&](std::size_t start, std::size_t end) {
        red.clear();
        green.clear();
        blue.clear();
        for (std::size_t candidate = start; candidate < end; ++candidate) {
            red.push_back(source[candidate].r);
            green.push_back(source[candidate].g);
            blue.push_back(source[candidate].b);
        }
        return Rgb{median(red), median(green), median(blue)};
    };

    // Find real pane/background boundaries first. An adjacent jump counts only
    // when each pixel also agrees with the median on its own side. A glyph edge
    // fails that test because both surrounding medians still describe the same
    // background. Median windows are then clipped to these boundaries.
    const auto protectedBoundaries = ProtectedCellBoundaries(
        protectedBoundaryPixels, cellWidth, source.size());
    std::vector<std::size_t> boundaries = protectedBoundaries;
    const float anchorThreshold = std::max(24.0f, edgeThreshold * 0.72f);
    for (std::size_t boundary = 1; boundary < source.size(); ++boundary) {
        if (std::binary_search(
                protectedBoundaries.begin(),
                protectedBoundaries.end(),
                boundary)) {
            continue;
        }
        if (ColorDistance(source[boundary - 1], source[boundary]) <
            edgeThreshold) {
            continue;
        }
        const auto nextProtected = std::upper_bound(
            protectedBoundaries.begin(),
            protectedBoundaries.end(),
            boundary);
        const std::size_t protectedEnd = *nextProtected;
        const std::size_t protectedStart = *(nextProtected - 1);
        // A genuine background division needs a full rejection radius of
        // evidence on both sides. Near a protected application edge, treat a
        // shorter excursion as foreground detail local to that application.
        if (boundary - protectedStart <
                static_cast<std::size_t>(radiusCells) ||
            protectedEnd - boundary <
                static_cast<std::size_t>(radiusCells)) {
            continue;
        }
        const std::size_t leftStart = std::max(
            protectedStart,
            boundary > static_cast<std::size_t>(radiusCells)
            ? boundary - static_cast<std::size_t>(radiusCells)
            : 0);
        const std::size_t rightEnd = std::min(
            protectedEnd,
            boundary + static_cast<std::size_t>(radiusCells));
        const Rgb leftBackground = medianColor(leftStart, boundary);
        const Rgb rightBackground = medianColor(boundary, rightEnd);
        if (ColorDistance(leftBackground, rightBackground) >= edgeThreshold &&
            ColorDistance(source[boundary - 1], leftBackground) <=
                anchorThreshold &&
            ColorDistance(source[boundary], rightBackground) <=
                anchorThreshold) {
            boundaries.push_back(boundary);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(
        std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    std::size_t segment = 0;
    for (std::size_t index = 0; index < source.size(); ++index) {
        while (segment + 1 < boundaries.size() &&
               index >= boundaries[segment + 1]) {
            ++segment;
        }
        const std::size_t segmentStart = boundaries[segment];
        const std::size_t segmentEnd = boundaries[segment + 1];
        const std::size_t start = index > static_cast<std::size_t>(radiusCells)
            ? index - static_cast<std::size_t>(radiusCells)
            : 0;
        const std::size_t end = std::min(
            source.size(), index + static_cast<std::size_t>(radiusCells) + 1);
        const Rgb background = medianColor(
            std::max(start, segmentStart), std::min(end, segmentEnd));
        if (ColorDistance(source[index], background) >= foregroundThreshold) {
            colors[index] = background;
        }
    }
}

}  // namespace

float ColorDistance(const Rgb& a, const Rgb& b) {
    // Red-mean distance tracks perceived RGB changes better than plain Euclidean
    // distance while staying cheap enough for every sampling frame.
    const float meanRed = (a.r + b.r) * 0.5f;
    const float dr = a.r - b.r;
    const float dg = a.g - b.g;
    const float db = a.b - b.b;
    const float redWeight = 2.0f + meanRed / 256.0f;
    const float blueWeight = 2.0f + (255.0f - meanRed) / 256.0f;
    return std::sqrt(redWeight * dr * dr + 4.0f * dg * dg + blueWeight * db * db) / 2.5f;
}

std::vector<Rgb> ExtractColorField(
    const std::uint8_t* bgra,
    int width,
    int height,
    int strideBytes,
    AdjacentEdge edge,
    const SamplerConfig& config) {
    if (!bgra || width <= 0 || height <= 0 || strideBytes < width * 4) {
        return {};
    }

    const int alongLength = LongAxisLength(width, height, edge);
    const int depthLength = DepthLength(width, height, edge);
    const int cellWidth = std::max(1, config.cellWidth);
    const int bins = std::max(2, std::min(32, config.colorBinsPerChannel));
    const int binCount = bins * bins * bins;
    const int cellCount = (alongLength + cellWidth - 1) / cellWidth;
    std::vector<Rgb> result(static_cast<std::size_t>(cellCount));
    std::vector<Bucket> histogram(static_cast<std::size_t>(binCount));
    std::vector<int> touchedBins;
    touchedBins.reserve(static_cast<std::size_t>(std::min(binCount, cellWidth * depthLength)));

    for (int cell = 0; cell < cellCount; ++cell) {
        touchedBins.clear();
        const int alongStart = cell * cellWidth;
        const int alongEnd = std::min(alongLength, alongStart + cellWidth);

        for (int along = alongStart; along < alongEnd; ++along) {
            for (int depth = 0; depth < depthLength; ++depth) {
                int x = 0;
                int y = 0;
                CoordinatesFor(along, depth, width, height, edge, x, y);
                const std::uint8_t* pixel = bgra + y * strideBytes + x * 4;
                const float b = static_cast<float>(pixel[0]);
                const float g = static_cast<float>(pixel[1]);
                const float r = static_cast<float>(pixel[2]);

                // Depth zero touches the taskbar. Normal sampling favors it so
                // nearby backgrounds extend naturally. Background-only mode uses
                // a much gentler bias so a caption or icon near that edge cannot
                // outweigh the larger background area in the sampling band.
                const float normalized = depthLength <= 1
                    ? 0.0f
                    : static_cast<float>(depth) / static_cast<float>(depthLength - 1);
                const float nearness = 1.0f - normalized;
                const float edgeBias = config.backgroundOnly ? 1.5f : 7.0f;
                const float weight =
                    1.0f + edgeBias * nearness * nearness * nearness;

                const int ri = std::min(bins - 1, static_cast<int>(r * bins / 256.0f));
                const int gi = std::min(bins - 1, static_cast<int>(g * bins / 256.0f));
                const int bi = std::min(bins - 1, static_cast<int>(b * bins / 256.0f));
                const int bucketIndex = (ri * bins + gi) * bins + bi;
                Bucket& bucket = histogram[static_cast<std::size_t>(bucketIndex)];
                if (bucket.weight == 0.0f) {
                    touchedBins.push_back(bucketIndex);
                }
                bucket.weight += weight;
                bucket.red += r * weight;
                bucket.green += g * weight;
                bucket.blue += b * weight;
            }
        }

        int winnerIndex = -1;
        for (int bucketIndex : touchedBins) {
            const Bucket& bucket = histogram[static_cast<std::size_t>(bucketIndex)];
            if (winnerIndex < 0 || bucket.weight > histogram[static_cast<std::size_t>(winnerIndex)].weight) {
                winnerIndex = bucketIndex;
            }
        }

        if (winnerIndex >= 0) {
            const Bucket& winner = histogram[static_cast<std::size_t>(winnerIndex)];
            result[static_cast<std::size_t>(cell)] = {
                winner.red / winner.weight,
                winner.green / winner.weight,
                winner.blue / winner.weight,
            };
        }

        for (int bucketIndex : touchedBins) {
            histogram[static_cast<std::size_t>(bucketIndex)] = Bucket{};
        }
    }

    if (config.backgroundOnly) {
        RemoveNarrowForegroundRuns(
            result,
            cellWidth,
            config.detailRejectionPixels,
            config.edgeThreshold,
            config.protectedBoundaryPixels);
    }

    if (result.size() < 2 || config.blurStrength <= 0.0f ||
        config.horizontalBlurPixels <= 0) {
        return result;
    }

    const int radiusCells = std::max(
        1,
        static_cast<int>(std::ceil(
            static_cast<float>(config.horizontalBlurPixels) / cellWidth)));
    const float sigma = std::max(0.75f, radiusCells * 0.52f);
    const auto protectedBoundaries = ProtectedCellBoundaries(
        config.protectedBoundaryPixels, cellWidth, result.size());
    std::vector<Rgb> smoothed = result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        Rgb weighted = result[i];
        float totalWeight = 1.0f;

        for (int direction : {-1, 1}) {
            int previous = static_cast<int>(i);
            for (int step = 1; step <= radiusCells; ++step) {
                const int candidate = static_cast<int>(i) + direction * step;
                if (candidate < 0 || candidate >= static_cast<int>(result.size())) {
                    break;
                }
                const std::size_t crossedBoundary = static_cast<std::size_t>(
                    direction > 0 ? candidate : previous);
                if (std::binary_search(
                        protectedBoundaries.begin(),
                        protectedBoundaries.end(),
                        crossedBoundary)) {
                    break;
                }
                if (ColorDistance(
                        result[static_cast<std::size_t>(previous)],
                        result[static_cast<std::size_t>(candidate)]) >
                    config.edgeThreshold) {
                    break;
                }

                const float distance = static_cast<float>(step);
                const float weight =
                    std::exp(-(distance * distance) / (2.0f * sigma * sigma));
                const Rgb& color = result[static_cast<std::size_t>(candidate)];
                weighted.r += color.r * weight;
                weighted.g += color.g * weight;
                weighted.b += color.b * weight;
                totalWeight += weight;
                previous = candidate;
            }
        }

        const Rgb average{
            weighted.r / totalWeight,
            weighted.g / totalWeight,
            weighted.b / totalWeight,
        };
        smoothed[i] = Mix(result[i], average, config.blurStrength);
    }
    return smoothed;
}

std::vector<std::uint32_t> ExpandColorField(
    const std::vector<Rgb>& cells,
    int outputLength,
    int cellWidth,
    float edgeThreshold,
    bool interpolate) {
    if (cells.empty() || outputLength <= 0) {
        return {};
    }

    cellWidth = std::max(1, cellWidth);
    std::vector<std::uint32_t> output(static_cast<std::size_t>(outputLength));
    for (int position = 0; position < outputLength; ++position) {
        const float cellPosition = (static_cast<float>(position) + 0.5f) / cellWidth - 0.5f;
        int left = static_cast<int>(std::floor(cellPosition));
        float fraction = cellPosition - std::floor(cellPosition);
        left = std::max(0, std::min(static_cast<int>(cells.size()) - 1, left));
        const int right = std::min(static_cast<int>(cells.size()) - 1, left + 1);

        Rgb color = cells[static_cast<std::size_t>(left)];
        if (interpolate && left != right &&
            ColorDistance(color, cells[static_cast<std::size_t>(right)]) <= edgeThreshold) {
            color = Mix(color, cells[static_cast<std::size_t>(right)], fraction);
        } else if (left != right && fraction >= 0.5f) {
            color = cells[static_cast<std::size_t>(right)];
        }

        const auto r = static_cast<std::uint32_t>(std::lround(ClampByte(color.r)));
        const auto g = static_cast<std::uint32_t>(std::lround(ClampByte(color.g)));
        const auto b = static_cast<std::uint32_t>(std::lround(ClampByte(color.b)));
        output[static_cast<std::size_t>(position)] = 0xFF000000u | (r << 16u) | (g << 8u) | b;
    }
    return output;
}

void AdjustColorField(
    std::vector<Rgb>& colors,
    float saturation,
    float brightness) {
    saturation = std::clamp(saturation, 0.0f, 2.0f);
    brightness = std::clamp(brightness, 0.5f, 1.5f);
    for (Rgb& color : colors) {
        const float luminance =
            0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
        color.r = ClampByte((luminance + (color.r - luminance) * saturation) * brightness);
        color.g = ClampByte((luminance + (color.g - luminance) * saturation) * brightness);
        color.b = ClampByte((luminance + (color.b - luminance) * saturation) * brightness);
    }
}

const std::vector<Rgb>& TemporalFilter::Update(
    const std::vector<Rgb>& target,
    double elapsedMilliseconds,
    double transitionMilliseconds) {
    if (target.empty()) {
        current_.clear();
        return current_;
    }
    if (current_.size() != target.size() || transitionMilliseconds <= 0.0) {
        current_ = target;
        return current_;
    }

    std::size_t changedCells = 0;
    for (std::size_t i = 0; i < target.size(); ++i) {
        if (ColorDistance(current_[i], target[i]) > 72.0f) {
            ++changedCells;
        }
    }

    const bool sceneChanged = changedCells * 100 >= target.size() * 42;
    const double elapsed = std::max(1.0, elapsedMilliseconds);
    const double tau = std::max(1.0, transitionMilliseconds);
    float amount = static_cast<float>(1.0 - std::exp(-elapsed / tau));
    if (sceneChanged) {
        // A window switch should feel immediate. Local motion (video, cursors,
        // animations) keeps the slower filter and therefore does not flicker.
        amount = std::max(amount, 0.62f);
    }

    for (std::size_t i = 0; i < target.size(); ++i) {
        current_[i] = Mix(current_[i], target[i], amount);
    }
    return current_;
}

void TemporalFilter::Reset() {
    current_.clear();
}

}  // namespace chameleon
