# ChameleonBar

ChameleonBar extends the pixels directly beside the Windows 11 taskbar into the taskbar itself. It is spatial rather than app-based: every narrow section follows the screen immediately above that section, so snapped windows and multi-monitor layouts can have different taskbar colors at the same time.

It is self-contained. It does not need Windhawk, TranslucentTB, a browser extension, an internet connection, or an installer.

## Run it

Ready-to-run binaries are in the [`dist/`](dist) folder. They are rebuilt and committed automatically by GitHub Actions on every push to `main`.

1. Copy everything in `dist/` into one folder. `ChameleonBar.exe` and `ChameleonBar.XamlBridge.dll` must stay together.
2. Turn off Windows 11 Taskbar Styler or any other tool that restyles the taskbar background.
3. Open `ChameleonBar.exe`. The status card in the bottom-left corner of the window should say **Live**.
4. Close the window. ChameleonBar keeps running in the notification area; double-click its icon to come back.

Right-click the tray icon and choose **Exit and restore taskbar** to stop. Pausing or exiting always hands the taskbar back to Windows.

If the status card says anything other than **Live**, see `Setup and Troubleshooting.txt`.

## Presets

| Preset | What it looks like |
| --- | --- |
| **Seamless** (default) | Pixel-exact match. Snapped windows keep crisp edges. |
| **Soft blend** | Neighbouring colors melt together for a smoother bar. |
| **Ambient glow** | Wide, saturated light, like a bias-lit monitor. |
| **Cinema** | Slow, steady changes. No flicker during video or games. |
| **Vivid** | Punchier color pulled from the same pixels. |
| **Muted** | Desaturated and dimmed for a quieter desktop. |
| **Battery saver** | Samples less often in larger blocks to save power. |

Moving any slider switches to **Custom**; moving it back to a preset's exact values reselects that preset.

## Settings

The window applies every change immediately and saves it automatically. A live strip at the top shows exactly what your main taskbar is displaying.

- **Look**: saturation, brightness, blend width and strength, transition time, a clear taskbar on an empty desktop, and holding colors while taskbar popups are open.
- **Sampling**: background-only filtering (ignores text, icons and dividers), how wide a detail to ignore, how tall and how far away the sampled strip is, update rate (16–250 ms), segment width and edge protection.
- **Contrast**: switches the clock, tray icons and running-app underlines between light and dark, with separate thresholds so they don't flicker.
- **General**: start with Windows, open hidden in the tray, restart Explorer, settings folder, setup guide, reset.

Keyboard: Tab moves between controls, arrow keys / Page Up / Page Down adjust sliders, Space toggles, Ctrl+Tab switches pages, Esc hides the window.

## How it works

- ChameleonBar captures a thin strip of the screen beside each taskbar, picks a dominant background color for every few pixels, and publishes one color per taskbar pixel through a small local shared-memory mapping.
- The included bridge is loaded into Explorer through Microsoft's XAML diagnostics API. It adds one ChameleonBar-owned rectangle directly above each taskbar's native background and paints the published colors into it. Because Windows never styles that element, opening Start, Search, Quick Settings or clicking the taskbar can't override it.
- Visible application edges are hard boundaries for filtering and blending, so split-screen edges stay sharp.
- A temporal filter reacts quickly to whole-window changes while damping local motion such as video.
- Tray and underline contrast is computed from the same published pixels, so it always matches what is painted.
- While Start, Quick Settings, previews, menus or tray flyouts are open next to the taskbar, sampling pauses so those surfaces never tint the bar.
- Explorer restarts, sign-in, display changes, DPI scaling and secondary taskbars are handled automatically; the bridge reconnects on its own.

Settings are stored in `%LocalAppData%\ChameleonBar\settings.ini`.

## Privacy and recovery

ChameleonBar has no networking code and no analytics. Screen samples stay in memory and are never saved. It doesn't replace system files or change your accent color.

If ChameleonBar pauses, exits or crashes, the bridge notices within a frame and restores the native taskbar. The bridge DLL stays loaded but dormant inside Explorer until Explorer next restarts. **General → Restart Explorer** (or the tray menu) restarts it cleanly if you ever need to.

The **Legacy transparency fallback** uses an undocumented Windows API, is off by default, and is only useful if the bridge can't load.

## Build

Install Visual Studio 2022 with the **Desktop development with C++** workload, then:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The files land in `build/bin/Release`. The GitHub Actions workflow runs the same build and publishes the runtime files as an artifact.

`build.ps1` is an alternative that uses the Clang/MinGW toolchain bundled with Windhawk as a compiler (the result has no Windhawk dependency). Use `build.ps1 -OutputDirectory some-folder` to build elsewhere.

## License and attribution

This project is GPL-3.0. The XAML diagnostics attachment in `bridge/xaml_bridge.cpp` is derived from **Windows 11 Taskbar Styler** by m417z/ramensoftware, which is also GPL-3.0. Attribution and source links are in `THIRD_PARTY_NOTICES.md`.
