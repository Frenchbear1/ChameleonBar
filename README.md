# ChameleonBar

ChameleonBar extends the pixels directly beside the Windows 11 taskbar into the taskbar itself. It is spatial rather than app-based: every narrow section follows the screen immediately above or beside that section, so snapped windows and multi-monitor layouts can have different taskbar colors at the same time.

This edition is self-contained. It does not require Windhawk, TranslucentTB, a browser extension, an internet connection, or an installer.

## Run it

1. Keep `ChameleonBar.exe` and `ChameleonBar.XamlBridge.dll` in the same folder.
2. Disable Windows 11 Taskbar Styler or any other tool currently controlling the taskbar's XAML background.
3. Open `ChameleonBar.exe`.
4. Adjust the live controls, then close or minimize the window to leave it running in the notification area.

Choose **Exit and restore taskbar** from the tray menu to stop. Pausing or exiting restores the original Windows taskbar fill and foreground.

See `Setup and Troubleshooting.txt` if the status line says the local bridge is unavailable.

## Controls

The settings popup applies changes immediately and saves them automatically.

- **Horizontal color blur width**: from crisp per-zone boundaries (`0 px`) to a wide color blend.
- **Blur strength**: mixes the unblurred local color back in without changing the blur radius.
- **Color transition time**: controls temporal smoothing; `0 ms` reacts instantly.
- **Saturation and brightness**: tune the generated color without affecting screen contents.
- **Fully transparent when only the desktop is visible**: hides the generated surface when no ordinary application window is visible on that monitor. Opening Start or Quick Settings does not count as opening an app.
- **Freeze colors while taskbar previews and popups are open**: retains the last application-derived strip while Start, Quick Settings, notifications, thumbnail previews, tray overflow, context menus, or taskbar-adjacent tray-app flyouts are visible.
- **Sampling zone depth and distance**: control how much of the screen beside the taskbar is examined and how far away it starts. Depth can be reduced to the physically meaningful minimum of one pixel.
- **Background colors only**: enabled by default; uses a median background estimate to reject mixed and antialiased text, icon, and divider colors.
- **Ignore foreground details up to**: controls the maximum width of a removable detail, now up to `192 px`. Every visible application's outer edges are hard boundaries, so a wide cleanup radius stays inside that window and cannot spread its color through a desktop gap or into a neighboring app.
- **Horizontal segment width**: trades maximum spatial precision for lower CPU use.
- **Split-window edge protection**: prevents colors from bleeding across strong window boundaries.
- **Update interval**: controls responsiveness and CPU use. `Instant` starts the next sample after the previous one finishes, while reserving a brief message-queue handoff so slider input and painting remain responsive and stale sample frames never queue up.
- **Adaptive contrast**: switches the tray arrow, network, volume, battery, clock, date, notifications, and each running-app underline between light and dark.
- **Separate light/dark thresholds**: create hysteresis so foreground colors do not flicker near the cutoff.
- **Start with Windows** starts the utility after sign-in. **When ChameleonBar starts, show only its tray icon** means it begins running without opening the settings window; double-click its chameleon icon to show settings.

The **Exact edges**, **Balanced**, and **Calm / video** profiles provide useful starting points. Moving any detailed control selects **Custom**.

## How it works

- The app publishes each taskbar's exact BGRA pixel strip through a local shared-memory surface.
- The local bridge applies that strip as the native taskbar XAML `ImageBrush`, so shell flyouts cannot move the color surface behind the wallpaper.
- A click-through backdrop window remains behind every taskbar as a compatibility fallback if the bridge is unavailable.
- Each monitor independently checks for a visible, non-minimized application. The backdrop stays present beneath shell flyouts while an app is open, but hides for a completely clear desktop.
- A narrow strip of the already-composited desktop directly beside each taskbar is captured locally.
- Small spatial cells choose a weighted dominant color. Pixels nearest the taskbar count more heavily, while tiny text and cursor pixels are rejected.
- Neighboring cells blend only when their colors are similar. Visible application rectangles create non-negotiable filter and blur boundaries, keeping split-screen edges and narrow desktop gaps sharp even at the `192 px` cleanup setting.
- A temporal filter reacts quickly to whole-window changes while damping localized video and animation noise.
- The included local XAML bridge changes only the native taskbar background and stroke, using the generated image while an app is visible and full transparency on an empty desktop.
- Start, Search, Quick Settings, notifications, and other shell flyouts cannot strand the adaptive surface in another window band because the image is part of the taskbar's own XAML tree.
- Taskbar-originated flyouts freeze sampling, preventing their white panels, thumbnails, icons, and accent colors from feeding back into the taskbar.
- Background-only mode uses an edge-preserving median estimate plus gentler near-edge weighting. Mixed antialiasing colors from letters and icons are rejected without blurring validated split-screen boundaries.
- The bridge samples the final composed pixels underneath the system tray, including ChameleonBar's brightness and blur adjustments, then explicitly updates both the tray theme and descendant text/icon foregrounds. Each running-app underline independently samples the taskbar color directly beneath its own button.
- Explorer restarts, display changes, top/side taskbars, DPI scaling, and secondary taskbars are rediscovered automatically.

Settings are stored in `%LocalAppData%\ChameleonBar\settings.ini`. The `[diagnostics]` section records the latest bridge status and tray-contrast result for troubleshooting.

The settings window uses a custom Windows 11-style dark interface while remaining a small dependency-free native application. Closing or minimizing it hides it to the notification area. The executable, window, and notification-area entry use the bundled ChameleonBar chameleon icon.

## Why the extracted theme behavior is transparent, not blurred

The uploaded Windows 11 Taskbar Styler source contains two relevant built-in themes:

- `TranslucentTaskbar` adds a fixed 18-pixel blur and dark tint.
- `SimplyTransparent` removes `BackgroundFill` and `BackgroundStroke` so another locally rendered surface can show through exactly.

ChameleonBar needs the second behavior. Reusing the fixed translucent brush would blur and darken the generated colors a second time, preventing an exact match and making split-screen edges bleed. The local bridge therefore retains only the XAML diagnostics attachment and the two transparency targets required for the transparent behavior, plus the adaptive foreground fixes for the tray and running indicators. It does not include Taskbar Styler's generic selector engine, theme catalog, Windhawk API calls, networking, or analytics.

## Privacy and recovery

ChameleonBar has no networking code and no analytics. Captured strips and sampled colors remain in memory and are never saved. The application does not replace system files or change the system-wide accent color.

The bridge is loaded into Explorer through Microsoft's XAML diagnostics API because Explorer owns the Windows 11 taskbar visuals. Pixel transfer uses a small local shared-memory mapping with no network or disk image storage. A named local event acts as a fail-safe: if ChameleonBar pauses, exits, or crashes, the bridge sees the event disappear and restores the original brushes and themes. The DLL can remain loaded but dormant until Explorer next restarts.

The optional **Legacy transparency fallback** uses an undocumented Windows composition attribute. It is off by default and is not needed when the local bridge is active.

## Build

For a normal public build, install Visual Studio 2022 with the **Desktop development with C++** workload, then run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The release files will be under `build/bin/Release`. The included GitHub Actions workflow performs the same build and publishes the two runtime files as an artifact.

`build.ps1` is a second convenience path for this development machine. It uses the Clang/MinGW toolchain bundled with Windhawk as a compiler, but the resulting program has no Windhawk runtime dependency.

The script produces:

- `ChameleonBar.exe`
- `ChameleonBar.XamlBridge.dll`
- `sampler_tests.exe`

Use `build.ps1 -OutputDirectory some-folder` to build somewhere other than `build`.

## License and attribution

This project is GPL-3.0. The focused XAML diagnostics attachment in `bridge/xaml_bridge.cpp` is derived from **Windows 11 Taskbar Styler** by m417z/ramensoftware, which is also GPL-3.0. Attribution and source links are in `THIRD_PARTY_NOTICES.md`.
