#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../src/color_sampler.h"
#include "../src/settings.h"
#include "../src/surface_publisher.h"
#include "../src/window_activity.h"

namespace {

struct Image {
    int width;
    int height;
    std::vector<std::uint8_t> bytes;

    Image(int w, int h) : width(w), height(h), bytes(static_cast<std::size_t>(w * h * 4), 0) {}

    void Set(int x, int y, int r, int g, int b) {
        const std::size_t offset = static_cast<std::size_t>((y * width + x) * 4);
        bytes[offset] = static_cast<std::uint8_t>(b);
        bytes[offset + 1] = static_cast<std::uint8_t>(g);
        bytes[offset + 2] = static_cast<std::uint8_t>(r);
        bytes[offset + 3] = 255;
    }
};

bool Near(float actual, float expected, float tolerance = 4.0f) {
    return std::abs(actual - expected) <= tolerance;
}

void Require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

chameleon::SamplerConfig TestConfig(int cellWidth) {
    chameleon::SamplerConfig config;
    config.cellWidth = cellWidth;
    config.blurStrength = 0.0f;
    config.horizontalBlurPixels = 0;
    config.edgeThreshold = 50.0f;
    return config;
}

void SolidColorIsPreserved() {
    Image image(40, 24);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            image.Set(x, y, 31, 122, 207);
        }
    }
    const auto colors = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, TestConfig(4));
    Require(colors.size() == 10, "solid field has expected number of cells");
    for (const auto& color : colors) {
        Require(Near(color.r, 31) && Near(color.g, 122) && Near(color.b, 207),
                "solid color survives extraction");
    }
}

void SplitBoundaryStaysSharp() {
    Image image(64, 32);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (x < 32) {
                image.Set(x, y, 19, 44, 81);
            } else {
                image.Set(x, y, 232, 225, 205);
            }
        }
    }
    const auto cells = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, TestConfig(4));
    const auto pixels = chameleon::ExpandColorField(cells, image.width, 4, 50.0f);
    Require((pixels[29] & 0x00FFFFFFu) == 0x00132C51u, "left side remains exact near split");
    Require((pixels[34] & 0x00FFFFFFu) == 0x00E8E1CDu, "right side remains exact near split");
}

void SparseTextDoesNotChangeBackground() {
    Image image(48, 48);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            image.Set(x, y, 245, 244, 240);
        }
    }
    for (int x = 2; x < image.width; x += 7) {
        for (int y = 3; y < image.height; y += 9) {
            image.Set(x, y, 20, 20, 20);
            if (x + 1 < image.width) {
                image.Set(x + 1, y, 20, 20, 20);
            }
        }
    }
    const auto colors = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, TestConfig(6));
    for (const auto& color : colors) {
        Require(color.r > 235 && color.g > 235 && color.b > 230,
                "sparse foreground detail is rejected");
    }
}

void BackgroundOnlyRemovesNarrowVerticalDetails() {
    Image image(96, 40);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            image.Set(x, y, 246, 246, 242);
        }
    }
    for (const int start : {20, 36, 52, 68}) {
        for (int x = start; x < start + 4; ++x) {
            for (int y = 0; y < image.height; ++y) {
                image.Set(x, y, 16, 18, 20);
            }
        }
    }

    auto config = TestConfig(2);
    config.backgroundOnly = true;
    config.detailRejectionPixels = 12;
    const auto filtered = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, config);
    for (std::size_t index = 0; index < filtered.size(); ++index) {
        const auto& color = filtered[index];
        if (!(color.r > 235 && color.g > 235 && color.b > 230)) {
            std::cerr << "detail cell " << index << ": "
                      << color.r << ',' << color.g << ',' << color.b << '\n';
        }
        Require(color.r > 235 && color.g > 235 && color.b > 230,
                "background-only mode removes narrow full-height glyph strokes");
    }

    config.backgroundOnly = false;
    const auto unfiltered = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, config);
    const bool retainedDetail = std::any_of(
        unfiltered.begin(), unfiltered.end(),
        [](const chameleon::Rgb& color) { return color.r < 40; });
    Require(retainedDetail, "turning background-only mode off keeps narrow detail");

    Image darkImage(96, 40);
    for (int y = 0; y < darkImage.height; ++y) {
        for (int x = 0; x < darkImage.width; ++x) {
            darkImage.Set(x, y, 18, 23, 31);
        }
    }
    for (const int start : {20, 36, 52, 68}) {
        for (int x = start; x < start + 4; ++x) {
            for (int y = 0; y < darkImage.height; ++y) {
                darkImage.Set(x, y, 238, 240, 242);
            }
        }
    }
    config.backgroundOnly = true;
    const auto darkFiltered = chameleon::ExtractColorField(
        darkImage.bytes.data(), darkImage.width, darkImage.height,
        darkImage.width * 4, chameleon::AdjacentEdge::Bottom, config);
    for (const auto& color : darkFiltered) {
        Require(color.r < 35 && color.g < 40 && color.b < 48,
                "background-only mode also removes light detail from dark backgrounds");
    }
}

void BackgroundOnlyRemovesDenseAntialiasedText() {
    Image image(180, 40);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (x < 90) {
                image.Set(x, y, 242, 242, 239);
            } else {
                image.Set(x, y, 24, 31, 43);
            }
        }
    }

    // Dense, alternating full-depth strokes reproduce the failure mode where
    // several pieces of one word survived the old run-by-run detector.
    for (int start = 12; start < 72; start += 8) {
        for (int x = start; x < start + 3; ++x) {
            for (int y = 0; y < image.height; ++y) {
                const int shade = (x == start || x == start + 2) ? 92 : 18;
                image.Set(x, y, shade, shade, shade);
            }
        }
    }
    for (int start = 108; start < 168; start += 8) {
        for (int x = start; x < start + 3; ++x) {
            for (int y = 0; y < image.height; ++y) {
                const int shade = (x == start || x == start + 2) ? 170 : 241;
                image.Set(x, y, shade, shade, shade);
            }
        }
    }

    auto config = TestConfig(1);
    config.backgroundOnly = true;
    config.detailRejectionPixels = 31;
    const auto filtered = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, config);
    for (std::size_t index = 0; index < filtered.size(); ++index) {
        const auto& color = filtered[index];
        if (index < 90) {
            if (!(color.r > 230 && color.g > 230 && color.b > 225)) {
                std::cerr << "light text cell " << index << ": "
                          << color.r << ',' << color.g << ',' << color.b << '\n';
            }
            Require(color.r > 230 && color.g > 230 && color.b > 225,
                    "dense dark text is rejected from a light background");
        } else {
            Require(color.r < 40 && color.g < 48 && color.b < 58,
                    "dense light text is rejected from a dark background");
        }
    }
}

void AdjacentPixelsHaveMoreInfluence() {
    Image image(16, 40);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            image.Set(x, y, 30, 160, 80);
        }
    }
    // The twelve rows touching the bottom taskbar use a different background.
    // Near-edge weighting makes that color win despite occupying less than half.
    for (int y = 28; y < 40; ++y) {
        for (int x = 0; x < image.width; ++x) {
            image.Set(x, y, 90, 70, 220);
        }
    }
    auto config = TestConfig(4);
    config.backgroundOnly = false;
    const auto colors = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, config);
    Require(colors.front().b > 200 && colors.front().r > 80,
            "the edge touching the taskbar is favored");
}

void SceneChangesReactQuickly() {
    chameleon::TemporalFilter filter;
    std::vector<chameleon::Rgb> dark(20, {10, 10, 10});
    std::vector<chameleon::Rgb> light(20, {240, 240, 240});
    filter.Update(dark, 50.0, 200.0);
    const auto& changed = filter.Update(light, 50.0, 200.0);
    Require(changed.front().r > 145, "whole-window scene change is accelerated");
}

void ZeroBlurUsesHardSegments() {
    const std::vector<chameleon::Rgb> cells{{12, 24, 36}, {220, 230, 240}};
    const auto pixels =
        chameleon::ExpandColorField(cells, 8, 4, 50.0f, false);
    Require((pixels[3] & 0x00FFFFFFu) == 0x000C1824u,
            "zero blur keeps the final pixel of a segment exact");
    Require((pixels[4] & 0x00FFFFFFu) == 0x00DCE6F0u,
            "zero blur switches cleanly at the next segment");
}

void WideBlurDoesNotCrossSplitBoundary() {
    Image image(96, 24);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (x < 48) {
                image.Set(x, y, 18, 37, 69);
            } else {
                image.Set(x, y, 238, 231, 210);
            }
        }
    }
    auto config = TestConfig(4);
    config.horizontalBlurPixels = 80;
    config.blurStrength = 1.0f;
    const auto cells = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, config);
    Require(cells[11].b < 90 && cells[11].r < 50,
            "wide blur stops on the dark side of a split boundary");
    Require(cells[12].r > 220 && cells[12].b > 190,
            "wide blur stops on the light side of a split boundary");
}

void WideDetailRejectionRespectsApplicationBoundaries() {
    Image image(260, 40);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (x < 140) {
                image.Set(x, y, 30, 70, 120);
            } else if (x < 164) {
                image.Set(x, y, 6, 8, 10);
            } else {
                image.Set(x, y, 220, 180, 100);
            }
        }
    }
    // A deliberately wide foreground block inside the first application is
    // eligible for removal at 192 px. Its median search must not pull either
    // the desktop gap or the neighboring application's color across an edge.
    for (int y = 0; y < image.height; ++y) {
        for (int x = 48; x < 108; ++x) {
            image.Set(x, y, 245, 245, 245);
        }
    }

    auto config = TestConfig(1);
    config.backgroundOnly = true;
    config.detailRejectionPixels = 192;
    config.protectedBoundaryPixels = {140, 164};
    const auto filtered = chameleon::ExtractColorField(
        image.bytes.data(), image.width, image.height, image.width * 4,
        chameleon::AdjacentEdge::Bottom, config);
    Require(
        filtered[72].r < 60 && filtered[72].g > 50 && filtered[72].b > 95,
        "wide detail rejection restores only the first application's background");
    Require(
        filtered[151].r < 20 && filtered[151].g < 20 && filtered[151].b < 20,
        "desktop gap remains isolated from both applications");
    Require(
        filtered[182].r > 200 && filtered[182].g > 160 && filtered[182].b > 80,
        "neighboring application remains isolated from the wide rejection radius");
}

void BenchmarkDesktopWidth() {
    Image image(1920, 56);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const int pane = x < image.width / 2 ? 0 : 1;
            image.Set(
                x,
                y,
                pane ? 235 - (y % 7) : 24 + (x % 5),
                pane ? 231 - (x % 5) : 42 + (y % 9),
                pane ? 220 : 71 + ((x + y) % 6));
        }
    }

    // Match the highest-detail live configuration used by the preview build.
    chameleon::SamplerConfig config = TestConfig(1);
    config.detailRejectionPixels = 31;
    config.blurStrength = 0.22f;
    config.horizontalBlurPixels = 8;
    const auto start = std::chrono::steady_clock::now();
    std::size_t checksum = 0;
    constexpr int iterations = 80;
    for (int i = 0; i < iterations; ++i) {
        const auto cells = chameleon::ExtractColorField(
            image.bytes.data(), image.width, image.height, image.width * 4,
            chameleon::AdjacentEdge::Bottom, config);
        checksum += cells.size();
    }
    const auto end = std::chrono::steady_clock::now();
    const double milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
    Require(checksum != 0, "benchmark output is consumed");
    std::cout << "Sampler benchmark (1920x56): "
              << (milliseconds / iterations) << " ms/frame\n";
}

chameleon::WindowActivitySnapshot AppWindow(
    const wchar_t* className,
    const wchar_t* processName) {
    chameleon::WindowActivitySnapshot window;
    window.visible = true;
    window.style = WS_OVERLAPPEDWINDOW;
    window.bounds = {100, 80, 1200, 850};
    window.className = className;
    window.processName = processName;
    return window;
}

void DesktopActivityClassification() {
    Require(
        chameleon::IsApplicationWindow(
            AppWindow(L"Chrome_WidgetWin_1", L"chrome.exe")),
        "ordinary browser window keeps the adaptive surface visible");
    Require(
        chameleon::IsApplicationWindow(
            AppWindow(L"CabinetWClass", L"explorer.exe")),
        "File Explorer counts as an application window");
    Require(
        !chameleon::IsApplicationWindow(
            AppWindow(
                L"Windows.UI.Core.CoreWindow",
                L"StartMenuExperienceHost.exe")),
        "Start does not make an empty desktop look occupied");
    Require(
        !chameleon::IsApplicationWindow(
            AppWindow(L"ControlCenterWindow", L"ShellExperienceHost.exe")),
        "Quick Settings does not make an empty desktop look occupied");
    Require(
        chameleon::IsTaskbarTransientWindow(
            AppWindow(L"ControlCenterWindow", L"ShellExperienceHost.exe")),
        "Quick Settings freezes taskbar color sampling");
    Require(
        chameleon::IsTaskbarTransientWindow(
            AppWindow(L"TaskListThumbnailWnd", L"explorer.exe")),
        "taskbar thumbnail previews freeze color sampling");
    Require(
        !chameleon::IsTaskbarTransientWindow(
            AppWindow(L"Chrome_WidgetWin_1", L"chrome.exe")),
        "ordinary application windows do not freeze color sampling");
    auto handwritingCanvas = AppWindow(
        L"ShellHandwritingCanvas {18E91349-E229-4995-8049-7437243BBCC3}",
        L"TabTip.exe");
    handwritingCanvas.style = WS_POPUP;
    handwritingCanvas.extendedStyle =
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    Require(
        !chameleon::IsTaskbarTransientWindow(handwritingCanvas),
        "invisible handwriting canvas does not permanently freeze sampling");
    Require(
        !chameleon::IsApplicationWindow(
            AppWindow(L"WorkerW", L"explorer.exe")),
        "desktop wallpaper windows are ignored");

    auto minimized = AppWindow(L"Notepad", L"notepad.exe");
    minimized.minimized = true;
    Require(
        !chameleon::IsApplicationWindow(minimized),
        "minimized applications allow desktop transparency");

    auto cloaked = AppWindow(L"Notepad", L"notepad.exe");
    cloaked.cloaked = true;
    Require(
        !chameleon::IsApplicationWindow(cloaked),
        "windows on another virtual desktop are ignored");

    const RECT app{100, 100, 900, 700};
    const RECT firstMonitor{0, 0, 1920, 1080};
    const RECT secondMonitor{1920, 0, 3840, 1080};
    Require(
        chameleon::SubstantiallyIntersects(app, firstMonitor),
        "application activates the taskbar on its monitor");
    Require(
        !chameleon::SubstantiallyIntersects(app, secondMonitor),
        "application does not activate an unrelated monitor");

    const RECT bottomTaskbar{0, 1032, 1920, 1080};
    const RECT quickSettings{1548, 632, 1908, 1020};
    const RECT distantPopup{1548, 300, 1908, 600};
    Require(
        chameleon::IsNearTaskbar(
            quickSettings, bottomTaskbar, firstMonitor),
        "bottom-right flyout is recognized as taskbar activity");
    Require(
        !chameleon::IsNearTaskbar(
            distantPopup, bottomTaskbar, firstMonitor),
        "unrelated distant popup does not freeze taskbar sampling");
}

void SharedSurfacePublishesExactPixels() {
    chameleon::SurfacePublisher publisher;
    Require(publisher.Initialize(), "shared XAML surface mapping opens");
    const HWND taskbar = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(0x1234));
    const RECT rectangle{0, 1032, 4, 1080};
    const std::vector<std::uint32_t> pixels{
        0xFF102030u, 0xFF405060u, 0xFF708090u, 0xFFA0B0C0u};
    Require(
        publisher.Publish(taskbar, rectangle, true, pixels),
        "application publishes a native taskbar surface");

    HANDLE mapping = OpenFileMappingW(
        FILE_MAP_READ, FALSE, chameleon::kSurfaceMappingName);
    Require(mapping != nullptr, "bridge can open the shared surface mapping");
    const auto* state = static_cast<const chameleon::SharedSurfaceState*>(
        MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            0,
            0,
            sizeof(chameleon::SharedSurfaceState)));
    Require(state != nullptr, "bridge can map the shared surface pixels");
    const auto handle = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(taskbar));
    const auto iterator = std::find_if(
        std::begin(state->slots),
        std::end(state->slots),
        [&](const chameleon::SharedSurfaceSlot& slot) {
            return slot.taskbarWindow == handle;
        });
    Require(iterator != std::end(state->slots), "published taskbar slot exists");
    Require(iterator->mode == chameleon::kSurfaceImage, "published taskbar surface is an image");
    Require(
        iterator->pixelWidth == 4 && iterator->pixelHeight == 1 &&
            iterator->pixelCount == 4,
        "published surface dimensions are exact");
    Require(
        std::equal(pixels.begin(), pixels.end(), iterator->pixels),
        "published surface preserves every BGRA pixel");
    publisher.SetNative(taskbar);
    Require(iterator->mode == chameleon::kSurfaceNative, "pausing hands the taskbar back to Windows");
    Require(
        publisher.Publish(taskbar, rectangle, true, pixels, chameleon::kSurfaceClear),
        "an empty desktop publishes a clear surface");
    Require(
        iterator->mode == chameleon::kSurfaceClear && iterator->pixelCount == 4,
        "a clear surface still carries pixels for tray contrast");

    publisher.SetContrast(true, 30, 20);
    Require(
        state->contrastEnabled == 1 && state->lightThresholdPercent == 30 &&
            state->darkThresholdPercent == 31,
        "contrast thresholds are published with hysteresis preserved");

    publisher.Release(taskbar);
    Require(iterator->taskbarWindow == 0, "a vanished taskbar frees its slot");

    // Explorer restarts create new taskbar windows. Stale slots must be
    // reclaimed instead of permanently filling the table.
    for (std::uintptr_t i = 0; i < chameleon::kMaximumTaskbars * 3; ++i) {
        const HWND fake = reinterpret_cast<HWND>(0x5000 + i * 0x10);
        Require(publisher.Publish(fake, rectangle, true, pixels),
                "slots of destroyed taskbars are reused");
    }
    UnmapViewOfFile(state);
    CloseHandle(mapping);
}

void PresetsRoundTrip() {
    for (const auto& preset : chameleon::Presets()) {
        chameleon::Settings settings;
        chameleon::ApplyPreset(settings, preset.id);
        Require(chameleon::MatchingPreset(settings) == preset.id,
                "each preset is recognized after it is applied");
        const auto tuning = chameleon::TuningForSettings(settings);
        Require(tuning.intervalMilliseconds >= chameleon::kMinimumIntervalMilliseconds,
                "preset update rates are within the supported range");
    }
    chameleon::Settings defaults;
    Require(chameleon::MatchingPreset(defaults) == chameleon::SamplingPreset::Seamless,
            "default settings are the Seamless preset");
    chameleon::Settings custom;
    chameleon::ApplyPreset(custom, chameleon::SamplingPreset::Vivid);
    custom.saturationPercent += 1;
    Require(chameleon::MatchingPreset(custom) == chameleon::SamplingPreset::Custom,
            "nudging a slider switches to Custom");
    chameleon::Settings legacyInstant;
    legacyInstant.intervalMilliseconds = 0;
    Require(chameleon::TuningForSettings(legacyInstant).intervalMilliseconds ==
                chameleon::kMinimumIntervalMilliseconds,
            "the retired Instant interval maps to the fastest supported rate");
}

}  // namespace

int main() {
    SolidColorIsPreserved();
    SplitBoundaryStaysSharp();
    SparseTextDoesNotChangeBackground();
    BackgroundOnlyRemovesNarrowVerticalDetails();
    BackgroundOnlyRemovesDenseAntialiasedText();
    AdjacentPixelsHaveMoreInfluence();
    SceneChangesReactQuickly();
    ZeroBlurUsesHardSegments();
    WideBlurDoesNotCrossSplitBoundary();
    WideDetailRejectionRespectsApplicationBoundaries();
    DesktopActivityClassification();
    SharedSurfacePublishesExactPixels();
    PresetsRoundTrip();
    BenchmarkDesktopWidth();
    std::cout << "All ChameleonBar sampler tests passed.\n";
    return 0;
}
