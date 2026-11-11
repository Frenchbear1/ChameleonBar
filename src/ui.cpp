// ChameleonBar settings window.
//
// A single custom-drawn, double-buffered window rendered with GDI+ so every
// control is antialiased, DPI-aware and flicker-free. There are no native
// child controls: layout, hit testing, keyboard focus and painting all come
// from one item list rebuilt for the current page.

#include "ui.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "resource.h"

namespace chameleon {
namespace {

namespace gp = Gdiplus;

constexpr wchar_t kVersion[] = L"1.0";

// ---------------------------------------------------------------------------
// Layout (device-independent pixels)
// ---------------------------------------------------------------------------

constexpr float kWindowWidth = 880.0f;
constexpr float kWindowHeight = 660.0f;
constexpr float kSidebarWidth = 228.0f;
constexpr float kPadding = 32.0f;
constexpr float kPreviewTop = 112.0f;
constexpr float kPreviewHeight = 26.0f;
constexpr float kScrollTop = 162.0f;
constexpr float kRowGap = 8.0f;
constexpr float kSliderRowHeight = 84.0f;
constexpr float kToggleRowHeight = 66.0f;
constexpr float kSectionHeight = 36.0f;
constexpr float kPresetCardHeight = 116.0f;

enum class Page { Presets = 0, Look, Sampling, Contrast, General, Count };

enum ControlId : int {
    kNone = 0,
    kNavBase = 100,
    kPauseButton = 200,
    kPresetBase = 300,  // + preset index; kPresetBase + 99 = custom
    kPresetCustom = kPresetBase + 99,
    kSaturation = 400,
    kBrightness,
    kBlurWidth,
    kBlurStrength,
    kTransition,
    kDepth,
    kOffset,
    kCellWidth,
    kEdge,
    kInterval,
    kDetail,
    kLightThreshold,
    kDarkThreshold,
    kClearDesktop = 500,
    kFreeze,
    kBackgroundOnly,
    kAdaptive,
    kStartup,
    kLaunchHidden,
    kLegacy,
    kRestartExplorer = 600,
    kOpenFolder,
    kSetupGuide,
    kReset,
};

enum class ValueFormat { Pixels, Percent, Milliseconds, Plain, Interval };

struct SliderSpec {
    int id;
    const wchar_t* label;
    const wchar_t* hint;
    int minimum;
    int maximum;
    int Settings::* member;
    ValueFormat format;
    bool zeroIsOff;
};

constexpr std::array<SliderSpec, 13> kSliders{{
    {kSaturation, L"Saturation", L"How colorful the taskbar is compared with the screen.",
     0, 200, &Settings::saturationPercent, ValueFormat::Percent, false},
    {kBrightness, L"Brightness", L"Dim or lift the generated color.",
     50, 150, &Settings::brightnessPercent, ValueFormat::Percent, false},
    {kBlurWidth, L"Blend width", L"How far colors spread sideways. Off keeps hard edges.",
     0, 200, &Settings::horizontalBlurPixels, ValueFormat::Pixels, true},
    {kBlurStrength, L"Blend strength", L"Mix between the exact color and the blended one.",
     0, 100, &Settings::blurStrengthPercent, ValueFormat::Percent, false},
    {kTransition, L"Transition time", L"Smoothing when colors change. Window switches stay fast.",
     0, 1000, &Settings::transitionMilliseconds, ValueFormat::Milliseconds, true},
    {kDepth, L"Sampling depth", L"How tall the strip of screen above the taskbar is.",
     1, 200, &Settings::sampleDepth, ValueFormat::Pixels, false},
    {kOffset, L"Sampling distance", L"Skip this many pixels first, e.g. to ignore a page footer.",
     0, 160, &Settings::sampleOffset, ValueFormat::Pixels, false},
    {kCellWidth, L"Segment width", L"Smaller is more precise; larger uses less CPU.",
     1, 24, &Settings::cellWidth, ValueFormat::Pixels, false},
    {kEdge, L"Edge protection", L"Higher lets more colors blend across window edges.",
     10, 180, &Settings::edgeThreshold, ValueFormat::Plain, false},
    {kInterval, L"Update rate", L"How often the screen is sampled.",
     kMinimumIntervalMilliseconds, kMaximumIntervalMilliseconds,
     &Settings::intervalMilliseconds, ValueFormat::Interval, false},
    {kDetail, L"Ignore details up to", L"Text, icons and dividers narrower than this are skipped.",
     0, 192, &Settings::detailRejectionPixels, ValueFormat::Pixels, true},
    {kLightThreshold, L"Switch to light glyphs below", L"Background brightness where white icons start.",
     1, 90, &Settings::lightGlyphThresholdPercent, ValueFormat::Percent, false},
    {kDarkThreshold, L"Switch back to dark glyphs above", L"Keep this above the first value to avoid flicker.",
     2, 95, &Settings::darkGlyphThresholdPercent, ValueFormat::Percent, false},
}};

const SliderSpec* FindSlider(int id) {
    for (const auto& spec : kSliders) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

struct ToggleSpec {
    int id;
    const wchar_t* label;
    const wchar_t* hint;
};

constexpr std::array<ToggleSpec, 7> kToggles{{
    {kClearDesktop, L"Clear taskbar on an empty desktop",
     L"See-through when no app window is open on that monitor."},
    {kFreeze, L"Hold colors while taskbar popups are open",
     L"Start, Quick Settings, previews and menus won't tint the bar."},
    {kBackgroundOnly, L"Background colors only",
     L"Ignore text, icons and thin lines so the bar doesn't shimmer."},
    {kAdaptive, L"Adaptive icon contrast",
     L"Switch the clock, tray icons and app underlines between light and dark."},
    {kStartup, L"Start with Windows", L"Run quietly in the tray after you sign in."},
    {kLaunchHidden, L"Open hidden in the tray",
     L"Don't show this window when ChameleonBar starts."},
    {kLegacy, L"Legacy transparency fallback",
     L"Only for systems where the bridge can't load. Usually leave off."},
}};

const ToggleSpec* FindToggle(int id) {
    for (const auto& spec : kToggles) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

struct PageInfo {
    const wchar_t* name;
    const wchar_t* title;
    const wchar_t* subtitle;
    wchar_t glyph;
};

constexpr std::array<PageInfo, 5> kPages{{
    {L"Presets", L"Presets", L"Pick a look. You can fine-tune anything afterwards.", L''},
    {L"Look", L"Look", L"Color, blending and behavior of the generated bar.", L''},
    {L"Sampling", L"Sampling", L"Which pixels are read and how often.", L''},
    {L"Contrast", L"Contrast", L"Keep the clock, tray and app underlines readable.", L''},
    {L"General", L"General", L"Startup, recovery and housekeeping.", L''},
}};

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

struct Palette {
    gp::ARGB background;
    gp::ARGB sidebar;
    gp::ARGB card;
    gp::ARGB cardHover;
    gp::ARGB border;
    gp::ARGB text;
    gp::ARGB muted;
    gp::ARGB faint;
    gp::ARGB accent;
    gp::ARGB accentHover;
    gp::ARGB onAccent;
    gp::ARGB track;
    gp::ARGB navSelected;
    gp::ARGB live;
    gp::ARGB waiting;
    gp::ARGB problem;
    COLORREF caption;
    bool dark;
};

constexpr gp::ARGB Rgb(std::uint32_t rgb, std::uint8_t alpha = 255) {
    return (static_cast<gp::ARGB>(alpha) << 24) | (rgb & 0xFFFFFFu);
}

const Palette kDarkPalette{
    Rgb(0x111318), Rgb(0x16191F), Rgb(0x1D2129), Rgb(0x242935), Rgb(0x2C323E),
    Rgb(0xF1F3F7), Rgb(0x9AA3B5), Rgb(0x5F6778), Rgb(0x5CE0BE), Rgb(0x78EACB),
    Rgb(0x06261E), Rgb(0x343B49), Rgb(0x232833), Rgb(0x5CE0BE), Rgb(0xF6C453),
    Rgb(0xFF7A7A), RGB(0x11, 0x13, 0x18), true};

const Palette kLightPalette{
    Rgb(0xF3F4F7), Rgb(0xEAECF0), Rgb(0xFFFFFF), Rgb(0xF6F7FA), Rgb(0xDADDE4),
    Rgb(0x16181D), Rgb(0x5E6676), Rgb(0x9AA1AE), Rgb(0x0E9C7C), Rgb(0x10B38E),
    Rgb(0xFFFFFF), Rgb(0xD5D9E0), Rgb(0xDDE0E7), Rgb(0x0E9C7C), Rgb(0xC98A00),
    Rgb(0xD64545), RGB(0xF3, 0xF4, 0xF7), false};

bool SystemUsesLightTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
            &size) != ERROR_SUCCESS) {
        return false;
    }
    return value != 0;
}

// ---------------------------------------------------------------------------
// DPI helpers (resolved dynamically so the app still starts on old builds)
// ---------------------------------------------------------------------------

UINT DpiForWindow(HWND window) {
    using Function = UINT(WINAPI*)(HWND);
    static const auto function = reinterpret_cast<Function>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (function && window) {
        if (const UINT dpi = function(window)) {
            return dpi;
        }
    }
    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

RECT WindowRectForClient(int width, int height, DWORD style, UINT dpi) {
    RECT rect{0, 0, width, height};
    using Function = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto function = reinterpret_cast<Function>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")));
    if (function) {
        function(&rect, style, FALSE, 0, dpi);
    } else {
        AdjustWindowRectEx(&rect, style, FALSE, 0);
    }
    return rect;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

std::unique_ptr<gp::GraphicsPath> RoundedPath(const gp::RectF& rect, float radius) {
    auto path = std::make_unique<gp::GraphicsPath>();
    radius = std::max(0.0f, std::min(radius, std::min(rect.Width, rect.Height) / 2.0f));
    if (radius <= 0.5f) {
        path->AddRectangle(rect);
        return path;
    }
    const float d = radius * 2.0f;
    path->AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path->AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.0f, 90.0f);
    path->AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.0f, 90.0f);
    path->AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

void FillRound(gp::Graphics& g, const gp::RectF& rect, float radius, gp::ARGB color) {
    gp::SolidBrush brush{gp::Color(color)};
    auto path = RoundedPath(rect, radius);
    g.FillPath(&brush, path.get());
}

void StrokeRound(gp::Graphics& g, const gp::RectF& rect, float radius,
                 gp::ARGB color, float width) {
    gp::Pen pen(gp::Color(color), width);
    gp::RectF inset = rect;
    inset.Inflate(-width / 2.0f, -width / 2.0f);
    auto path = RoundedPath(inset, radius);
    g.DrawPath(&pen, path.get());
}

enum TextFlags : unsigned {
    kLeft = 0,
    kCenter = 1,
    kRight = 2,
    kTop = 0,
    kMiddle = 4,
    kWrap = 8,
};

void Text(gp::Graphics& g, const std::wstring& text, const gp::RectF& rect,
          const gp::Font& font, gp::ARGB color, unsigned flags = kLeft | kMiddle) {
    gp::StringFormat format;
    format.SetAlignment((flags & kCenter) ? gp::StringAlignmentCenter
                        : (flags & kRight) ? gp::StringAlignmentFar
                                           : gp::StringAlignmentNear);
    format.SetLineAlignment((flags & kMiddle) ? gp::StringAlignmentCenter
                                              : gp::StringAlignmentNear);
    format.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    if (!(flags & kWrap)) {
        format.SetFormatFlags(gp::StringFormatFlagsNoWrap);
    }
    gp::SolidBrush brush{gp::Color(color)};
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, rect, &format, &brush);
}

float TextWidth(gp::Graphics& g, const std::wstring& text, const gp::Font& font) {
    gp::RectF bounds;
    gp::StringFormat format(gp::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | gp::StringFormatFlagsMeasureTrailingSpaces);
    g.MeasureString(text.c_str(), static_cast<INT>(text.size()), &font,
                    gp::PointF(0, 0), &format, &bounds);
    return bounds.Width;
}

gp::ARGB Blend(gp::ARGB a, gp::ARGB b, float amount) {
    const auto channel = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<gp::ARGB>(std::lround(x + (y - x) * amount)) << shift;
    };
    return channel(24) | channel(16) | channel(8) | channel(0);
}

// Little illustrative "taskbars" for the preset cards.
struct Swatch {
    std::array<std::uint32_t, 5> colors;
    bool hardEdges;
};

Swatch SwatchFor(SamplingPreset preset) {
    switch (preset) {
    case SamplingPreset::Seamless:
        return {{0x2B5876, 0x2B5876, 0xE8E1CD, 0xE8E1CD, 0x4E4376}, true};
    case SamplingPreset::Soft:
        return {{0x2B5876, 0x3F7896, 0x86A8B0, 0xC9C6B8, 0xE8E1CD}, false};
    case SamplingPreset::Ambient:
        return {{0x7F00FF, 0xB400F0, 0xE100FF, 0xFF4F7B, 0xFF8A00}, false};
    case SamplingPreset::Cinema:
        return {{0x0F2027, 0x16303A, 0x203A43, 0x264A57, 0x2C5364}, false};
    case SamplingPreset::Vivid:
        return {{0xFF1F5A, 0xFF7A00, 0xFFC400, 0x00C2FF, 0x6A5CFF}, false};
    case SamplingPreset::Muted:
        return {{0x5B616C, 0x6E747F, 0x858B95, 0x989DA6, 0xA9ADB4}, false};
    case SamplingPreset::Eco:
        return {{0x1E5631, 0x2E7D46, 0x4C9A61, 0x76B485, 0xA4DE02}, true};
    default:
        return {{0x444A57, 0x5C6475, 0x747D90, 0x5C6475, 0x444A57}, false};
    }
}

void DrawSwatch(gp::Graphics& g, const gp::RectF& rect, const Swatch& swatch, float radius) {
    auto path = RoundedPath(rect, radius);
    gp::Region oldClip;
    g.GetClip(&oldClip);
    g.SetClip(path.get(), gp::CombineModeIntersect);
    if (swatch.hardEdges) {
        const float width = rect.Width / static_cast<float>(swatch.colors.size());
        for (std::size_t i = 0; i < swatch.colors.size(); ++i) {
            gp::SolidBrush brush{gp::Color(Rgb(swatch.colors[i]))};
            g.FillRectangle(&brush, rect.X + width * static_cast<float>(i) - 0.5f,
                            rect.Y, width + 1.0f, rect.Height);
        }
    } else {
        gp::RectF gradientRect = rect;
        gradientRect.Inflate(1.0f, 0.0f);
        gp::LinearGradientBrush brush(gradientRect, gp::Color(Rgb(swatch.colors.front())),
                                      gp::Color(Rgb(swatch.colors.back())),
                                      gp::LinearGradientModeHorizontal);
        std::array<gp::Color, 5> colors;
        std::array<gp::REAL, 5> positions{0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        for (std::size_t i = 0; i < colors.size(); ++i) {
            colors[i] = gp::Color(Rgb(swatch.colors[i]));
        }
        brush.SetInterpolationColors(colors.data(), positions.data(),
                                     static_cast<INT>(colors.size()));
        g.FillRectangle(&brush, gradientRect);
    }
    g.SetClip(&oldClip);
}

std::wstring FormatValue(const SliderSpec& spec, int value) {
    if (spec.zeroIsOff && value == 0) {
        return L"Off";
    }
    switch (spec.format) {
    case ValueFormat::Pixels:
        return std::to_wstring(value) + L" px";
    case ValueFormat::Percent:
        return std::to_wstring(value) + L"%";
    case ValueFormat::Milliseconds:
        return std::to_wstring(value) + L" ms";
    case ValueFormat::Interval: {
        const int fps = static_cast<int>(std::lround(1000.0 / std::max(1, value)));
        return std::to_wstring(value) + L" ms · " + std::to_wstring(fps) + L" fps";
    }
    case ValueFormat::Plain:
    default:
        return std::to_wstring(value);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Window implementation
// ---------------------------------------------------------------------------

struct SettingsWindow::Impl {
    enum class Kind { Section, Slider, Toggle, Button, Preset, Note };

    struct Item {
        Kind kind;
        int id;
        gp::RectF rect;  // content coordinates (physical px, before scrolling)
        std::wstring text;
    };

    HINSTANCE instance = nullptr;
    SettingsHost* host = nullptr;
    HWND hwnd = nullptr;
    SettingsWindow* owner = nullptr;

    float scale = 1.0f;
    const Palette* palette = &kDarkPalette;
    Page page = Page::Presets;
    std::vector<Item> items;
    float contentHeight = 0.0f;
    float scroll = 0.0f;

    int hover = kNone;
    int pressed = kNone;
    int dragging = kNone;
    int focus = kNone;
    bool keyboardFocusVisible = false;
    bool tracking = false;

    // Back buffer.
    HDC bufferDc = nullptr;
    HBITMAP bufferBitmap = nullptr;
    HGDIOBJ bufferOld = nullptr;
    int bufferWidth = 0;
    int bufferHeight = 0;

    std::unique_ptr<gp::Font> fontBody;
    std::unique_ptr<gp::Font> fontStrong;
    std::unique_ptr<gp::Font> fontSmall;
    std::unique_ptr<gp::Font> fontTitle;
    std::unique_ptr<gp::Font> fontBrand;
    std::unique_ptr<gp::Font> fontSection;
    std::unique_ptr<gp::Font> fontIcon;
    std::unique_ptr<gp::Font> fontIconSmall;

    UiPreview preview;

    float S(float value) const { return value * scale; }

    // -- geometry ------------------------------------------------------------
    float ClientWidth() const {
        RECT client{};
        GetClientRect(hwnd, &client);
        return static_cast<float>(client.right - client.left);
    }
    float ClientHeight() const {
        RECT client{};
        GetClientRect(hwnd, &client);
        return static_cast<float>(client.bottom - client.top);
    }
    float ContentLeft() const { return S(kSidebarWidth) + S(kPadding); }
    float ContentRight() const { return ClientWidth() - S(kPadding); }
    float ScrollTop() const { return S(kScrollTop); }
    float ScrollBottom() const { return ClientHeight() - S(20.0f); }
    float ViewHeight() const { return std::max(0.0f, ScrollBottom() - ScrollTop()); }
    float MaxScroll() const { return std::max(0.0f, contentHeight - ViewHeight()); }

    gp::RectF NavRect(int index) const {
        return gp::RectF(S(14.0f), S(96.0f) + S(44.0f) * static_cast<float>(index),
                         S(kSidebarWidth - 28.0f), S(38.0f));
    }
    gp::RectF StatusRect() const {
        const float height = S(118.0f);
        return gp::RectF(S(14.0f), ClientHeight() - S(14.0f) - S(44.0f) - S(10.0f) - height,
                         S(kSidebarWidth - 28.0f), height);
    }
    gp::RectF PauseRect() const {
        return gp::RectF(S(14.0f), ClientHeight() - S(14.0f) - S(44.0f),
                         S(kSidebarWidth - 28.0f), S(44.0f));
    }
    gp::RectF PreviewRect() const {
        return gp::RectF(ContentLeft(), S(kPreviewTop), ContentRight() - ContentLeft(),
                         S(kPreviewHeight));
    }

    // Converts an item's content rectangle to client coordinates.
    gp::RectF OnScreen(const gp::RectF& rect) const {
        return gp::RectF(rect.X, rect.Y + ScrollTop() - scroll, rect.Width, rect.Height);
    }

    gp::RectF SliderTrack(const gp::RectF& row) const {
        return gp::RectF(row.X + S(18.0f), row.Y + row.Height - S(24.0f),
                         row.Width - S(36.0f), S(4.0f));
    }
    gp::RectF ToggleSwitch(const gp::RectF& row) const {
        const float width = S(44.0f);
        const float height = S(22.0f);
        return gp::RectF(row.X + row.Width - S(18.0f) - width,
                         row.Y + (row.Height - height) / 2.0f, width, height);
    }

    // -- settings access -----------------------------------------------------
    int SliderValue(const SliderSpec& spec) const {
        return host->EditableSettings().*(spec.member);
    }

    bool ToggleValue(int id) const {
        const Settings& settings = host->EditableSettings();
        switch (id) {
        case kClearDesktop: return settings.transparentOnDesktop;
        case kFreeze: return settings.freezeOnTaskbarActivity;
        case kBackgroundOnly: return settings.backgroundOnly;
        case kAdaptive: return settings.adaptiveTrayContrast;
        case kStartup: return host->StartsWithWindows();
        case kLaunchHidden: return settings.launchHidden;
        case kLegacy: return settings.autoTransparency;
        default: return false;
        }
    }

    void SetSliderValue(const SliderSpec& spec, int value) {
        Settings& settings = host->EditableSettings();
        value = std::clamp(value, spec.minimum, spec.maximum);
        if (settings.*(spec.member) == value) {
            return;
        }
        settings.*(spec.member) = value;
        if (spec.id == kLightThreshold &&
            settings.lightGlyphThresholdPercent >= settings.darkGlyphThresholdPercent) {
            settings.darkGlyphThresholdPercent =
                std::min(95, settings.lightGlyphThresholdPercent + 1);
        } else if (spec.id == kDarkThreshold &&
                   settings.darkGlyphThresholdPercent <= settings.lightGlyphThresholdPercent) {
            settings.lightGlyphThresholdPercent =
                std::max(1, settings.darkGlyphThresholdPercent - 1);
        }
        host->SettingsEdited();
        Invalidate();
    }

    void Toggle(int id) {
        Settings& settings = host->EditableSettings();
        switch (id) {
        case kClearDesktop: settings.transparentOnDesktop = !settings.transparentOnDesktop; break;
        case kFreeze: settings.freezeOnTaskbarActivity = !settings.freezeOnTaskbarActivity; break;
        case kBackgroundOnly: settings.backgroundOnly = !settings.backgroundOnly; break;
        case kAdaptive: settings.adaptiveTrayContrast = !settings.adaptiveTrayContrast; break;
        case kLaunchHidden: settings.launchHidden = !settings.launchHidden; break;
        case kLegacy: settings.autoTransparency = !settings.autoTransparency; break;
        case kStartup:
            host->SetStartsWithWindows(!host->StartsWithWindows());
            Invalidate();
            return;
        default: return;
        }
        host->SettingsEdited();
        Invalidate();
    }

    void Activate(int id) {
        if (id >= kNavBase && id < kNavBase + static_cast<int>(Page::Count)) {
            SetPage(static_cast<Page>(id - kNavBase));
            return;
        }
        if (id == kPauseButton) {
            host->SetPaused(!host->IsPaused());
            Invalidate();
            return;
        }
        if (id == kPresetCustom) {
            SetPage(Page::Look);
            return;
        }
        if (id >= kPresetBase && id < kPresetBase + static_cast<int>(Presets().size())) {
            host->ChoosePreset(Presets()[static_cast<std::size_t>(id - kPresetBase)].id);
            Invalidate();
            return;
        }
        if (FindToggle(id)) {
            Toggle(id);
            return;
        }
        switch (id) {
        case kRestartExplorer: host->Perform(UiAction::RestartExplorer); break;
        case kOpenFolder: host->Perform(UiAction::OpenSettingsFolder); break;
        case kSetupGuide: host->Perform(UiAction::OpenSetupGuide); break;
        case kReset: host->Perform(UiAction::ResetDefaults); break;
        default: break;
        }
        Invalidate();
    }

    void SetPage(Page next) {
        if (page != next) {
            page = next;
            scroll = 0.0f;
            focus = kNone;
        }
        Layout();
        Invalidate();
    }

    // -- layout --------------------------------------------------------------
    void Layout() {
        items.clear();
        const float left = ContentLeft();
        const float width = ContentRight() - left;
        float y = 0.0f;

        const auto section = [&](const wchar_t* title) {
            if (!items.empty()) {
                y += S(10.0f);
            }
            items.push_back({Kind::Section, kNone, gp::RectF(left, y, width, S(kSectionHeight)), title});
            y += S(kSectionHeight);
        };
        const auto slider = [&](int id) {
            items.push_back({Kind::Slider, id, gp::RectF(left, y, width, S(kSliderRowHeight)), {}});
            y += S(kSliderRowHeight + kRowGap);
        };
        const auto toggle = [&](int id) {
            items.push_back({Kind::Toggle, id, gp::RectF(left, y, width, S(kToggleRowHeight)), {}});
            y += S(kToggleRowHeight + kRowGap);
        };
        const auto note = [&](const std::wstring& text, float height) {
            items.push_back({Kind::Note, kNone, gp::RectF(left, y, width, S(height)), text});
            y += S(height);
        };

        switch (page) {
        case Page::Presets: {
            const float gap = S(12.0f);
            const float cardWidth = (width - gap) / 2.0f;
            const auto presets = Presets();
            for (std::size_t i = 0; i < presets.size(); ++i) {
                const float x = left + (i % 2 == 0 ? 0.0f : cardWidth + gap);
                items.push_back({Kind::Preset, kPresetBase + static_cast<int>(i),
                                 gp::RectF(x, y, cardWidth, S(kPresetCardHeight)), {}});
                if (i % 2 == 1) {
                    y += S(kPresetCardHeight) + gap;
                }
            }
            const float x = left + (presets.size() % 2 == 0 ? 0.0f : cardWidth + gap);
            items.push_back({Kind::Preset, kPresetCustom,
                             gp::RectF(x, y, cardWidth, S(kPresetCardHeight)), {}});
            y += S(kPresetCardHeight) + gap;
            break;
        }
        case Page::Look:
            section(L"Color");
            slider(kSaturation);
            slider(kBrightness);
            section(L"Blending");
            slider(kBlurWidth);
            slider(kBlurStrength);
            slider(kTransition);
            section(L"Behavior");
            toggle(kClearDesktop);
            toggle(kFreeze);
            break;
        case Page::Sampling:
            section(L"What gets read");
            toggle(kBackgroundOnly);
            slider(kDetail);
            slider(kDepth);
            slider(kOffset);
            section(L"Precision and speed");
            slider(kInterval);
            slider(kCellWidth);
            slider(kEdge);
            break;
        case Page::Contrast:
            section(L"Tray and clock");
            toggle(kAdaptive);
            slider(kLightThreshold);
            slider(kDarkThreshold);
            note(L"Contrast follows the exact colors painted under each icon, "
                 L"including saturation and brightness.", 48.0f);
            break;
        case Page::General: {
            section(L"Startup");
            toggle(kStartup);
            toggle(kLaunchHidden);
            section(L"Recovery");
            const std::array<std::pair<int, const wchar_t*>, 4> buttons{{
                {kRestartExplorer, L"Restart Explorer"},
                {kOpenFolder, L"Settings folder"},
                {kSetupGuide, L"Setup guide"},
                {kReset, L"Reset everything"},
            }};
            const float gap = S(10.0f);
            const float buttonWidth = (width - gap) / 2.0f;
            for (std::size_t i = 0; i < buttons.size(); ++i) {
                const float x = left + (i % 2 == 0 ? 0.0f : buttonWidth + gap);
                items.push_back({Kind::Button, buttons[i].first,
                                 gp::RectF(x, y, buttonWidth, S(42.0f)), buttons[i].second});
                if (i % 2 == 1) {
                    y += S(42.0f) + gap;
                }
            }
            toggle(kLegacy);
            section(L"About");
            note(std::wstring(L"ChameleonBar ") + kVersion +
                     L". Everything runs locally: no network access, no analytics, "
                     L"and screen samples never leave memory.",
                 52.0f);
            break;
        }
        default:
            break;
        }
        contentHeight = y + S(8.0f);
        scroll = std::clamp(scroll, 0.0f, MaxScroll());
    }

    std::vector<int> FocusOrder() const {
        std::vector<int> order;
        for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
            order.push_back(kNavBase + i);
        }
        for (const Item& item : items) {
            if (item.kind == Kind::Slider || item.kind == Kind::Toggle ||
                item.kind == Kind::Button || item.kind == Kind::Preset) {
                order.push_back(item.id);
            }
        }
        order.push_back(kPauseButton);
        return order;
    }

    const Item* FindItem(int id) const {
        for (const Item& item : items) {
            if (item.id == id && item.kind != Kind::Section && item.kind != Kind::Note) {
                return &item;
            }
        }
        return nullptr;
    }

    void ScrollIntoView(int id) {
        const Item* item = FindItem(id);
        if (!item) {
            return;
        }
        if (item->rect.Y < scroll) {
            scroll = item->rect.Y - S(8.0f);
        } else if (item->rect.Y + item->rect.Height > scroll + ViewHeight()) {
            scroll = item->rect.Y + item->rect.Height - ViewHeight() + S(8.0f);
        }
        scroll = std::clamp(scroll, 0.0f, MaxScroll());
    }

    int HitTest(float x, float y) const {
        for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
            if (NavRect(i).Contains(x, y)) {
                return kNavBase + i;
            }
        }
        if (PauseRect().Contains(x, y)) {
            return kPauseButton;
        }
        if (y < ScrollTop() || y > ScrollBottom() || x < S(kSidebarWidth)) {
            return kNone;
        }
        for (const Item& item : items) {
            if (item.kind == Kind::Section || item.kind == Kind::Note) {
                continue;
            }
            if (OnScreen(item.rect).Contains(x, y)) {
                return item.id;
            }
        }
        return kNone;
    }

    // -- resources -----------------------------------------------------------
    std::unique_ptr<gp::Font> MakeFont(std::initializer_list<const wchar_t*> families,
                                       float size, INT style) {
        for (const wchar_t* family : families) {
            gp::FontFamily fontFamily(family);
            if (fontFamily.GetLastStatus() != gp::Ok) {
                continue;
            }
            auto font = std::make_unique<gp::Font>(&fontFamily, S(size), style, gp::UnitPixel);
            if (font->GetLastStatus() == gp::Ok) {
                return font;
            }
        }
        return nullptr;
    }

    void CreateFonts() {
        fontBody = MakeFont({L"Segoe UI", L"Tahoma", L"Arial"}, 13.5f, gp::FontStyleRegular);
        fontSmall = MakeFont({L"Segoe UI", L"Tahoma", L"Arial"}, 12.0f, gp::FontStyleRegular);
        fontStrong = MakeFont({L"Segoe UI Semibold", L"Segoe UI", L"Tahoma", L"Arial"}, 14.0f,
                              gp::FontStyleRegular);
        if (fontStrong && !fontBody) {
            fontBody = MakeFont({L"Arial"}, 13.5f, gp::FontStyleRegular);
        }
        fontTitle = MakeFont({L"Segoe UI Semibold", L"Segoe UI", L"Tahoma", L"Arial"}, 26.0f,
                             gp::FontStyleRegular);
        fontBrand = MakeFont({L"Segoe UI Semibold", L"Segoe UI", L"Tahoma", L"Arial"}, 17.0f,
                             gp::FontStyleRegular);
        fontSection = MakeFont({L"Segoe UI Semibold", L"Segoe UI", L"Tahoma", L"Arial"}, 12.0f,
                               gp::FontStyleRegular);
        fontIcon = MakeFont({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}, 16.0f,
                            gp::FontStyleRegular);
        fontIconSmall = MakeFont({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}, 11.0f,
                                 gp::FontStyleRegular);
    }

    bool FontsReady() const {
        return fontBody && fontSmall && fontStrong && fontTitle && fontBrand && fontSection;
    }

    void ApplyTheme() {
        palette = SystemUsesLightTheme() ? &kLightPalette : &kDarkPalette;
        const BOOL dark = palette->dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        const COLORREF caption = palette->caption;
        DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
        const int corner = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    }

    void UpdateScale() {
        scale = static_cast<float>(DpiForWindow(hwnd)) / 96.0f;
        CreateFonts();
    }

    void EnsureBuffer(int width, int height) {
        if (bufferDc && width == bufferWidth && height == bufferHeight) {
            return;
        }
        ReleaseBuffer();
        HDC screen = GetDC(hwnd);
        bufferDc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = std::max(1, width);
        info.bmiHeader.biHeight = -std::max(1, height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bufferBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(hwnd, screen);
        bufferOld = SelectObject(bufferDc, bufferBitmap);
        bufferWidth = width;
        bufferHeight = height;
    }

    void ReleaseBuffer() {
        if (bufferDc) {
            SelectObject(bufferDc, bufferOld);
            DeleteDC(bufferDc);
        }
        if (bufferBitmap) {
            DeleteObject(bufferBitmap);
        }
        bufferDc = nullptr;
        bufferBitmap = nullptr;
        bufferOld = nullptr;
        bufferWidth = 0;
        bufferHeight = 0;
    }

    void Invalidate() {
        if (hwnd) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    // -- painting ------------------------------------------------------------
    void Paint(HDC target) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) {
            return;
        }
        EnsureBuffer(width, height);
        {
            gp::Graphics g(bufferDc);
            g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
            g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
            g.Clear(gp::Color(palette->background));
            if (FontsReady()) {
                PaintSidebar(g);
                PaintHeader(g);
                PaintContent(g);
            }
        }
        if (FontsReady()) {
            // The application icon keeps its alpha when drawn with GDI.
            HICON icon = static_cast<HICON>(LoadImageW(
                instance, MAKEINTRESOURCEW(IDI_CHAMELEONBAR), IMAGE_ICON,
                static_cast<int>(S(32.0f)), static_cast<int>(S(32.0f)), LR_DEFAULTCOLOR));
            if (icon) {
                DrawIconEx(bufferDc, static_cast<int>(S(22.0f)), static_cast<int>(S(26.0f)), icon,
                           static_cast<int>(S(32.0f)), static_cast<int>(S(32.0f)), 0, nullptr,
                           DI_NORMAL);
                DestroyIcon(icon);
            }
        }
        BitBlt(target, 0, 0, width, height, bufferDc, 0, 0, SRCCOPY);
    }

    void PaintFocus(gp::Graphics& g, const gp::RectF& rect, float radius, int id) {
        if (keyboardFocusVisible && focus == id) {
            gp::RectF ring = rect;
            ring.Inflate(S(3.0f), S(3.0f));
            StrokeRound(g, ring, radius + S(3.0f), palette->accent, S(2.0f));
        }
    }

    void PaintSidebar(gp::Graphics& g) {
        const Palette& p = *palette;
        gp::SolidBrush sidebar{gp::Color(p.sidebar)};
        g.FillRectangle(&sidebar, 0.0f, 0.0f, S(kSidebarWidth), ClientHeight());

        Text(g, L"ChameleonBar", gp::RectF(S(62.0f), S(24.0f), S(160.0f), S(22.0f)),
             *fontBrand, p.text);
        Text(g, std::wstring(L"Version ") + kVersion,
             gp::RectF(S(62.0f), S(45.0f), S(160.0f), S(16.0f)), *fontSmall, p.muted);

        for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
            const gp::RectF rect = NavRect(i);
            const bool selected = static_cast<int>(page) == i;
            const bool hovered = hover == kNavBase + i;
            if (selected || hovered) {
                FillRound(g, rect, S(8.0f), selected ? p.navSelected : Blend(p.sidebar, p.navSelected, 0.6f));
            }
            if (selected) {
                FillRound(g, gp::RectF(rect.X, rect.Y + S(10.0f), S(3.0f), rect.Height - S(20.0f)),
                          S(1.5f), p.accent);
            }
            if (fontIcon) {
                Text(g, std::wstring(1, kPages[static_cast<std::size_t>(i)].glyph),
                     gp::RectF(rect.X + S(12.0f), rect.Y, S(22.0f), rect.Height), *fontIcon,
                     selected ? p.accent : p.muted, kCenter | kMiddle);
            }
            Text(g, kPages[static_cast<std::size_t>(i)].name,
                 gp::RectF(rect.X + S(44.0f), rect.Y, rect.Width - S(48.0f), rect.Height),
                 selected ? *fontStrong : *fontBody, selected ? p.text : p.muted);
            PaintFocus(g, rect, S(8.0f), kNavBase + i);
        }

        // Status card.
        const UiStatus status = host->Status();
        const gp::RectF card = StatusRect();
        FillRound(g, card, S(10.0f), p.card);
        gp::ARGB dot = p.waiting;
        switch (status.level) {
        case UiStatus::Level::Live: dot = p.live; break;
        case UiStatus::Level::Paused: dot = p.muted; break;
        case UiStatus::Level::Problem: dot = p.problem; break;
        case UiStatus::Level::Waiting: dot = p.waiting; break;
        }
        const float dotSize = S(9.0f);
        gp::SolidBrush dotBrush{gp::Color(dot)};
        if (status.level == UiStatus::Level::Live) {
            gp::SolidBrush halo{gp::Color(Rgb(dot & 0xFFFFFF, 60))};
            g.FillEllipse(&halo, card.X + S(14.0f) - S(3.5f), card.Y + S(18.0f) - S(3.5f),
                          dotSize + S(7.0f), dotSize + S(7.0f));
        }
        g.FillEllipse(&dotBrush, card.X + S(14.0f), card.Y + S(18.0f), dotSize, dotSize);
        Text(g, status.headline,
             gp::RectF(card.X + S(32.0f), card.Y + S(10.0f), card.Width - S(42.0f), S(24.0f)),
             *fontStrong, p.text);
        Text(g, status.detail,
             gp::RectF(card.X + S(14.0f), card.Y + S(40.0f), card.Width - S(26.0f),
                       card.Height - S(48.0f)),
             *fontSmall, p.muted, kLeft | kTop | kWrap);

        // Pause / resume.
        const gp::RectF pause = PauseRect();
        const bool paused = host->IsPaused();
        const bool hovered = hover == kPauseButton;
        const bool down = pressed == kPauseButton;
        gp::ARGB fill = paused ? p.accent : p.card;
        if (hovered) fill = paused ? p.accentHover : p.cardHover;
        if (down) fill = Blend(fill, p.background, 0.3f);
        FillRound(g, pause, S(10.0f), fill);
        if (!paused) {
            StrokeRound(g, pause, S(10.0f), p.border, S(1.0f));
        }
        const gp::ARGB labelColor = paused ? p.onAccent : p.text;
        const std::wstring label = paused ? L"Resume color matching" : L"Pause";
        float iconWidth = fontIconSmall ? S(22.0f) : 0.0f;
        const float labelWidth = TextWidth(g, label, *fontStrong);
        const float startX = pause.X + (pause.Width - labelWidth - iconWidth) / 2.0f;
        if (fontIconSmall) {
            Text(g, paused ? L"" : L"",
                 gp::RectF(startX, pause.Y, S(16.0f), pause.Height), *fontIconSmall,
                 labelColor, kCenter | kMiddle);
        }
        Text(g, label, gp::RectF(startX + iconWidth - S(8.0f), pause.Y, labelWidth + S(16.0f), pause.Height),
             *fontStrong, labelColor, kCenter | kMiddle);
        PaintFocus(g, pause, S(10.0f), kPauseButton);
    }

    void PaintHeader(gp::Graphics& g) {
        const Palette& p = *palette;
        const PageInfo& info = kPages[static_cast<std::size_t>(page)];
        const float left = ContentLeft();
        const float width = ContentRight() - left;
        Text(g, info.title, gp::RectF(left, S(22.0f), width, S(36.0f)), *fontTitle, p.text);
        Text(g, info.subtitle, gp::RectF(left, S(60.0f), width, S(20.0f)), *fontBody, p.muted);

        // Live preview of what the primary taskbar is showing right now.
        const gp::RectF bar = PreviewRect();
        Text(g, L"LIVE TASKBAR", gp::RectF(left, bar.Y - S(22.0f), width / 2.0f, S(18.0f)),
             *fontSection, p.faint);
        std::wstring state;
        if (host->IsPaused()) {
            state = L"Paused";
        } else if (preview.pixels.empty()) {
            state = L"Waiting for a window";
        } else if (preview.frozen) {
            state = L"Holding while a popup is open";
        } else if (preview.clear) {
            state = L"Clear (empty desktop)";
        } else {
            state = L"Matching";
        }
        Text(g, state, gp::RectF(left + width / 2.0f, bar.Y - S(22.0f), width / 2.0f, S(18.0f)),
             *fontSmall, p.muted, kRight | kMiddle);

        const float radius = S(7.0f);
        FillRound(g, bar, radius, p.card);
        if (!preview.pixels.empty() && !host->IsPaused()) {
            const int count = static_cast<int>(preview.pixels.size());
            gp::Bitmap strip(count, 1, count * 4, PixelFormat32bppRGB,
                             reinterpret_cast<BYTE*>(preview.pixels.data()));
            auto path = RoundedPath(bar, radius);
            gp::Region oldClip;
            g.GetClip(&oldClip);
            g.SetClip(path.get(), gp::CombineModeIntersect);
            const auto oldInterpolation = g.GetInterpolationMode();
            g.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
            g.DrawImage(&strip, bar, 0.0f, 0.0f, static_cast<gp::REAL>(count), 1.0f, gp::UnitPixel);
            g.SetInterpolationMode(oldInterpolation);
            if (preview.clear) {
                // Diagonal hatch: the real taskbar is see-through here.
                gp::HatchBrush hatch(gp::HatchStyleWideUpwardDiagonal,
                                     gp::Color(Rgb(0xFFFFFF, 46)), gp::Color(0, 0, 0, 0));
                g.FillRectangle(&hatch, bar);
            }
            g.SetClip(&oldClip);
        }
        StrokeRound(g, bar, radius, p.border, S(1.0f));
    }

    void PaintContent(gp::Graphics& g) {
        const Palette& p = *palette;
        gp::Region oldClip;
        g.GetClip(&oldClip);
        g.SetClip(gp::RectF(S(kSidebarWidth), ScrollTop() - S(4.0f),
                            ClientWidth() - S(kSidebarWidth), ViewHeight() + S(8.0f)),
                  gp::CombineModeIntersect);
        for (const Item& item : items) {
            const gp::RectF rect = OnScreen(item.rect);
            if (rect.Y + rect.Height < ScrollTop() - S(10.0f) ||
                rect.Y > ScrollBottom() + S(10.0f)) {
                continue;
            }
            switch (item.kind) {
            case Kind::Section:
                Text(g, item.text, gp::RectF(rect.X + S(2.0f), rect.Y + S(8.0f), rect.Width, rect.Height - S(8.0f)),
                     *fontSection, p.muted, kLeft | kMiddle);
                break;
            case Kind::Note:
                Text(g, item.text, gp::RectF(rect.X + S(2.0f), rect.Y + S(6.0f), rect.Width - S(4.0f), rect.Height - S(6.0f)),
                     *fontSmall, p.muted, kLeft | kTop | kWrap);
                break;
            case Kind::Slider:
                PaintSlider(g, item, rect);
                break;
            case Kind::Toggle:
                PaintToggle(g, item, rect);
                break;
            case Kind::Button:
                PaintButton(g, item, rect);
                break;
            case Kind::Preset:
                PaintPreset(g, item, rect);
                break;
            }
        }
        g.SetClip(&oldClip);

        // Scroll indicator.
        if (MaxScroll() > 0.5f) {
            const float trackHeight = ViewHeight();
            const float thumbHeight = std::max(S(32.0f), trackHeight * ViewHeight() / contentHeight);
            const float thumbY = ScrollTop() + (trackHeight - thumbHeight) * (scroll / MaxScroll());
            FillRound(g, gp::RectF(ClientWidth() - S(10.0f), thumbY, S(4.0f), thumbHeight),
                      S(2.0f), Rgb(p.muted & 0xFFFFFF, 110));
        }
    }

    void PaintRowBackground(gp::Graphics& g, const gp::RectF& rect, int id) {
        const Palette& p = *palette;
        const bool hovered = hover == id || dragging == id;
        FillRound(g, rect, S(10.0f), hovered ? p.cardHover : p.card);
        StrokeRound(g, rect, S(10.0f), Blend(p.border, p.card, 0.35f), S(1.0f));
    }

    void PaintSlider(gp::Graphics& g, const Item& item, const gp::RectF& rect) {
        const SliderSpec* spec = FindSlider(item.id);
        if (!spec) {
            return;
        }
        const Palette& p = *palette;
        PaintRowBackground(g, rect, item.id);
        const int value = SliderValue(*spec);
        const std::wstring valueText = FormatValue(*spec, value);
        const float valueWidth = TextWidth(g, valueText, *fontStrong) + S(18.0f);
        Text(g, spec->label,
             gp::RectF(rect.X + S(18.0f), rect.Y + S(12.0f), rect.Width - S(36.0f) - valueWidth, S(22.0f)),
             *fontStrong, p.text);
        Text(g, valueText,
             gp::RectF(rect.X + rect.Width - S(18.0f) - valueWidth, rect.Y + S(12.0f), valueWidth, S(22.0f)),
             *fontStrong, dragging == item.id ? p.accent : p.text, kRight | kMiddle);
        Text(g, spec->hint,
             gp::RectF(rect.X + S(18.0f), rect.Y + S(34.0f), rect.Width - S(36.0f), S(18.0f)),
             *fontSmall, p.muted);

        const gp::RectF track = SliderTrack(rect);
        FillRound(g, track, track.Height / 2.0f, p.track);
        const float amount = spec->maximum > spec->minimum
            ? static_cast<float>(value - spec->minimum) /
                  static_cast<float>(spec->maximum - spec->minimum)
            : 0.0f;
        const float thumbX = track.X + track.Width * std::clamp(amount, 0.0f, 1.0f);
        if (thumbX > track.X + 0.5f) {
            FillRound(g, gp::RectF(track.X, track.Y, thumbX - track.X, track.Height),
                      track.Height / 2.0f, p.accent);
        }
        const float centerY = track.Y + track.Height / 2.0f;
        const bool active = dragging == item.id || hover == item.id;
        const float outer = S(active ? 10.0f : 9.0f);
        const float inner = S(dragging == item.id ? 6.0f : (active ? 5.5f : 4.5f));
        gp::SolidBrush ring{gp::Color(p.dark ? Rgb(0x2E3440) : Rgb(0xFFFFFF))};
        gp::Pen ringPen(gp::Color(p.border), S(1.0f));
        g.FillEllipse(&ring, thumbX - outer, centerY - outer, outer * 2.0f, outer * 2.0f);
        g.DrawEllipse(&ringPen, thumbX - outer, centerY - outer, outer * 2.0f, outer * 2.0f);
        gp::SolidBrush knob{gp::Color(p.accent)};
        g.FillEllipse(&knob, thumbX - inner, centerY - inner, inner * 2.0f, inner * 2.0f);
        PaintFocus(g, rect, S(10.0f), item.id);
    }

    void PaintToggle(gp::Graphics& g, const Item& item, const gp::RectF& rect) {
        const ToggleSpec* spec = FindToggle(item.id);
        if (!spec) {
            return;
        }
        const Palette& p = *palette;
        PaintRowBackground(g, rect, item.id);
        const gp::RectF knobArea = ToggleSwitch(rect);
        Text(g, spec->label,
             gp::RectF(rect.X + S(18.0f), rect.Y + S(12.0f), knobArea.X - rect.X - S(30.0f), S(22.0f)),
             *fontStrong, p.text);
        Text(g, spec->hint,
             gp::RectF(rect.X + S(18.0f), rect.Y + S(34.0f), knobArea.X - rect.X - S(30.0f), S(18.0f)),
             *fontSmall, p.muted);

        const bool on = ToggleValue(item.id);
        const bool hovered = hover == item.id;
        const float radius = knobArea.Height / 2.0f;
        if (on) {
            FillRound(g, knobArea, radius, hovered ? p.accentHover : p.accent);
        } else {
            FillRound(g, knobArea, radius, hovered ? p.cardHover : p.card);
            StrokeRound(g, knobArea, radius, p.muted, S(1.5f));
        }
        const float knob = on ? S(14.0f) : S(12.0f);
        const float knobX = on ? knobArea.X + knobArea.Width - S(5.0f) - knob
                               : knobArea.X + S(5.0f);
        gp::SolidBrush knobBrush{gp::Color(on ? p.onAccent : p.muted)};
        g.FillEllipse(&knobBrush, knobX, knobArea.Y + (knobArea.Height - knob) / 2.0f, knob, knob);
        PaintFocus(g, rect, S(10.0f), item.id);
    }

    void PaintButton(gp::Graphics& g, const Item& item, const gp::RectF& rect) {
        const Palette& p = *palette;
        const bool hovered = hover == item.id;
        const bool down = pressed == item.id;
        const bool destructive = item.id == kReset;
        gp::ARGB fill = hovered ? p.cardHover : p.card;
        if (down) fill = Blend(fill, p.background, 0.35f);
        FillRound(g, rect, S(8.0f), fill);
        StrokeRound(g, rect, S(8.0f), p.border, S(1.0f));
        Text(g, item.text, rect, *fontStrong, destructive ? p.problem : p.text, kCenter | kMiddle);
        PaintFocus(g, rect, S(8.0f), item.id);
    }

    void PaintPreset(gp::Graphics& g, const Item& item, const gp::RectF& rect) {
        const Palette& p = *palette;
        const Settings& settings = host->EditableSettings();
        const bool custom = item.id == kPresetCustom;
        const PresetInfo* info = custom
            ? nullptr
            : &Presets()[static_cast<std::size_t>(item.id - kPresetBase)];
        const SamplingPreset id = custom ? SamplingPreset::Custom : info->id;
        const bool selected = settings.preset == id;
        const bool hovered = hover == item.id;
        const bool down = pressed == item.id;

        gp::ARGB fill = hovered ? p.cardHover : p.card;
        if (down) fill = Blend(fill, p.background, 0.3f);
        FillRound(g, rect, S(12.0f), fill);
        if (selected) {
            StrokeRound(g, rect, S(12.0f), p.accent, S(2.0f));
        } else {
            StrokeRound(g, rect, S(12.0f), Blend(p.border, p.card, 0.35f), S(1.0f));
        }

        const std::wstring name = custom ? L"Custom" : info->name;
        const std::wstring description = custom
            ? (selected ? L"Your own tuning is active. Adjust it on the other pages."
                        : L"Changing any slider switches here automatically.")
            : info->description;
        Text(g, name, gp::RectF(rect.X + S(16.0f), rect.Y + S(12.0f), rect.Width - S(56.0f), S(22.0f)),
             *fontStrong, p.text);
        Text(g, description,
             gp::RectF(rect.X + S(16.0f), rect.Y + S(36.0f), rect.Width - S(32.0f), S(40.0f)),
             *fontSmall, p.muted, kLeft | kTop | kWrap);
        DrawSwatch(g, gp::RectF(rect.X + S(16.0f), rect.Y + rect.Height - S(26.0f),
                                rect.Width - S(32.0f), S(12.0f)),
                   SwatchFor(id), S(6.0f));

        if (selected) {
            const float size = S(22.0f);
            const gp::RectF badge(rect.X + rect.Width - S(14.0f) - size, rect.Y + S(12.0f), size, size);
            gp::SolidBrush brush{gp::Color(p.accent)};
            g.FillEllipse(&brush, badge);
            if (fontIconSmall) {
                Text(g, L"", badge, *fontIconSmall, p.onAccent, kCenter | kMiddle);
            }
        }
        PaintFocus(g, rect, S(12.0f), item.id);
    }

    // -- input ---------------------------------------------------------------
    void DragSlider(int id, float x) {
        const SliderSpec* spec = FindSlider(id);
        const Item* item = FindItem(id);
        if (!spec || !item) {
            return;
        }
        const gp::RectF track = SliderTrack(OnScreen(item->rect));
        const float amount = std::clamp((x - track.X) / std::max(1.0f, track.Width), 0.0f, 1.0f);
        const int value = spec->minimum +
            static_cast<int>(std::lround(amount * static_cast<float>(spec->maximum - spec->minimum)));
        SetSliderValue(*spec, value);
    }

    void OnMouseMove(float x, float y) {
        if (!tracking) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            tracking = true;
        }
        if (dragging != kNone) {
            DragSlider(dragging, x);
            return;
        }
        const int hit = HitTest(x, y);
        if (hit != hover) {
            hover = hit;
            Invalidate();
        }
    }

    void OnMouseDown(float x, float y) {
        SetFocus(hwnd);
        keyboardFocusVisible = false;
        const int hit = HitTest(x, y);
        if (hit == kNone) {
            return;
        }
        focus = hit;
        if (FindSlider(hit)) {
            dragging = hit;
            SetCapture(hwnd);
            DragSlider(hit, x);
        } else {
            pressed = hit;
            SetCapture(hwnd);
        }
        Invalidate();
    }

    void OnMouseUp(float x, float y) {
        // Read the state first: ReleaseCapture() sends WM_CAPTURECHANGED,
        // which clears it.
        const int wasDragging = dragging;
        const int was = pressed;
        dragging = kNone;
        pressed = kNone;
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (wasDragging == kNone && was != kNone && HitTest(x, y) == was) {
            Activate(was);
        }
        Invalidate();
    }

    void OnWheel(int delta) {
        if (MaxScroll() <= 0.0f) {
            return;
        }
        scroll = std::clamp(scroll - static_cast<float>(delta) / WHEEL_DELTA * S(60.0f), 0.0f,
                            MaxScroll());
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd, &cursor);
        hover = HitTest(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        Invalidate();
    }

    void MoveFocus(int direction) {
        const auto order = FocusOrder();
        if (order.empty()) {
            return;
        }
        auto it = std::find(order.begin(), order.end(), focus);
        int index = it == order.end()
            ? (direction > 0 ? 0 : static_cast<int>(order.size()) - 1)
            : static_cast<int>(it - order.begin()) + direction;
        index = (index + static_cast<int>(order.size())) % static_cast<int>(order.size());
        focus = order[static_cast<std::size_t>(index)];
        ScrollIntoView(focus);
        keyboardFocusVisible = true;
        Invalidate();
    }

    bool OnKey(WPARAM key) {
        const bool control = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        if (key == VK_TAB && control) {
            const int count = static_cast<int>(Page::Count);
            SetPage(static_cast<Page>((static_cast<int>(page) + (shift ? count - 1 : 1)) % count));
            return true;
        }
        if (key == VK_TAB) {
            MoveFocus(shift ? -1 : 1);
            return true;
        }
        if (key == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
            return true;
        }
        if (const SliderSpec* spec = FindSlider(focus)) {
            const int range = spec->maximum - spec->minimum;
            const int big = std::max(1, range / 10);
            int value = SliderValue(*spec);
            switch (key) {
            case VK_LEFT: case VK_DOWN: value -= 1; break;
            case VK_RIGHT: case VK_UP: value += 1; break;
            case VK_PRIOR: value += big; break;
            case VK_NEXT: value -= big; break;
            case VK_HOME: value = spec->minimum; break;
            case VK_END: value = spec->maximum; break;
            default: return false;
            }
            keyboardFocusVisible = true;
            SetSliderValue(*spec, value);
            return true;
        }
        if ((key == VK_SPACE || key == VK_RETURN) && focus != kNone) {
            keyboardFocusVisible = true;
            Activate(focus);
            return true;
        }
        if (key == VK_UP || key == VK_DOWN) {
            MoveFocus(key == VK_UP ? -1 : 1);
            return true;
        }
        return false;
    }
};

namespace {

LRESULT CALLBACK SettingsWindowProcedure(HWND window, UINT message, WPARAM wParam,
                                         LPARAM lParam) {
    auto* impl = reinterpret_cast<SettingsWindow::Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        impl = static_cast<SettingsWindow::Impl*>(create->lpCreateParams);
        impl->hwnd = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    if (!impl) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_CREATE:
        impl->UpdateScale();
        impl->ApplyTheme();
        impl->Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        impl->Paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        impl->Layout();
        impl->Invalidate();
        return 0;
    case WM_DPICHANGED: {
        impl->UpdateScale();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        impl->Layout();
        impl->Invalidate();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        impl->ApplyTheme();
        impl->Invalidate();
        break;
    case WM_MOUSEMOVE:
        impl->OnMouseMove(static_cast<float>(GET_X_LPARAM(lParam)),
                          static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_MOUSELEAVE:
        impl->tracking = false;
        if (impl->dragging == kNone && impl->hover != kNone) {
            impl->hover = kNone;
            impl->Invalidate();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        impl->OnMouseDown(static_cast<float>(GET_X_LPARAM(lParam)),
                          static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_LBUTTONUP:
        impl->OnMouseUp(static_cast<float>(GET_X_LPARAM(lParam)),
                        static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_CAPTURECHANGED:
        if (impl->dragging != kNone || impl->pressed != kNone) {
            impl->dragging = kNone;
            impl->pressed = kNone;
            impl->Invalidate();
        }
        return 0;
    case WM_MOUSEWHEEL:
        impl->OnWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (impl->OnKey(wParam)) {
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, impl->hover != kNone ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const RECT minimum = WindowRectForClient(
            static_cast<int>(impl->S(720.0f)), static_cast<int>(impl->S(520.0f)),
            WS_OVERLAPPEDWINDOW, DpiForWindow(window));
        info->ptMinTrackSize.x = minimum.right - minimum.left;
        info->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        impl->ReleaseBuffer();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

bool SettingsWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = SettingsWindowProcedure;
    windowClass.lpszClassName = kClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CHAMELEONBAR), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CHAMELEONBAR), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
    windowClass.hbrBackground = nullptr;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

SettingsWindow::SettingsWindow(HINSTANCE instance, SettingsHost* host)
    : impl_(new Impl()) {
    impl_->instance = instance;
    impl_->host = host;
    impl_->owner = this;
}

SettingsWindow::~SettingsWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
    delete impl_;
}

void SettingsWindow::Show() {
    if (!hwnd_) {
        constexpr DWORD style = WS_OVERLAPPEDWINDOW;
        // Create on the monitor under the cursor, then size for its DPI.
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        hwnd_ = CreateWindowExW(
            0, kClassName, L"ChameleonBar", style,
            info.rcWork.left + 40, info.rcWork.top + 40, 100, 100,
            nullptr, nullptr, impl_->instance, impl_);
        if (!hwnd_) {
            return;
        }
        const UINT dpi = DpiForWindow(hwnd_);
        const float scale = static_cast<float>(dpi) / 96.0f;
        const RECT frame = WindowRectForClient(
            static_cast<int>(kWindowWidth * scale), static_cast<int>(kWindowHeight * scale), style, dpi);
        const int width = std::min<int>(frame.right - frame.left, info.rcWork.right - info.rcWork.left);
        const int height = std::min<int>(frame.bottom - frame.top, info.rcWork.bottom - info.rcWork.top);
        const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
        const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2;
        SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    impl_->Layout();
    impl_->host->Preview(impl_->preview);
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    impl_->Invalidate();
}

void SettingsWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

bool SettingsWindow::Visible() const {
    return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
}

void SettingsWindow::Refresh() {
    if (hwnd_) {
        impl_->Layout();
        impl_->Invalidate();
    }
}

void SettingsWindow::Tick() {
    if (!Visible()) {
        return;
    }
    impl_->host->Preview(impl_->preview);
    // Only the header strip and the sidebar status change on their own.
    const RECT header{
        static_cast<LONG>(impl_->ContentLeft()), static_cast<LONG>(impl_->S(80.0f)),
        static_cast<LONG>(impl_->ClientWidth()), static_cast<LONG>(impl_->S(kPreviewTop + kPreviewHeight + 4.0f))};
    InvalidateRect(hwnd_, &header, FALSE);
    const gp::RectF status = impl_->StatusRect();
    const RECT sidebar{0, static_cast<LONG>(status.Y) - 2, static_cast<LONG>(impl_->S(kSidebarWidth)),
                       static_cast<LONG>(impl_->ClientHeight())};
    InvalidateRect(hwnd_, &sidebar, FALSE);
}

namespace {
ULONG_PTR g_gdiplusToken = 0;
}

bool StartGraphics() {
    gp::GdiplusStartupInput input;
    return gp::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == gp::Ok;
}

void StopGraphics() {
    if (g_gdiplusToken) {
        gp::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

}  // namespace chameleon
