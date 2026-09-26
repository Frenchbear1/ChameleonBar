# Changelog

## 0.9.1 local preview

- Keep `Instant` sampling to a single one-shot request at a time and yield between completed frames so input and paint messages cannot be starved.
- Restart the sampling scheduler only when the update-interval control actually changes.
- Debounce settings-file writes during slider drags, then flush them when interaction stops or the settings window closes.

## 0.9 local preview

- Remove the experimental Animations page and its window-capture/overlay code.
- Apply adaptive light/dark contrast to every native running-app underline as well as the system tray, clock, and notification glyphs.
- Extend foreground-detail rejection from 96 px to 192 px while hard-clipping filtering and blur at each visible application's bounds, including narrow desktop gaps between windows.
- Add an `Instant` update mode that schedules the next sample as soon as the current one completes without queueing stale frames.
- Reduce sampling-zone depth to a one-pixel minimum.
- Replace the ambiguous launch-hidden label with an explanation that only the tray icon is shown at startup.
- Add an original multi-resolution chameleon icon to the executable, title bar, taskbar, and notification area.

## 0.8 local preview

- Add an Animations page with independently selectable minimize, close, and drifting-fragment effects plus a live 180–2000 ms duration control.
- Reproduce Simpl 1.7.1's window-vanish motion locally: the same 820 ms default, `cubic-bezier(.17,.84,.22,1)` easing, four opacity/blur/saturation/scale/translation/clipping keyframes, and fragment timing.
- Cache only the current foreground application in memory so a closing window can finish animating after Windows destroys its native surface; screenshots are never written to disk.
- Replace every standard checkbox and the native profile combo with custom-painted controls matching the rest of the interface.
- Space slider labels and tracks independently so descenders cannot be clipped on any page.
- Double-buffer slider painting and stop rebuilding every control during a drag, eliminating frozen white trails.

## 0.7 local preview

- Move the generated per-pixel color strip into the native taskbar XAML background through a local shared-memory `WriteableBitmap`/`ImageBrush`, eliminating the shell window-band failure exposed by Start and Quick Settings.
- Retain the separate click-through backdrop only as a fallback when the local XAML bridge is unavailable.
- Freeze sampling while Start, Search, Quick Settings, notification panels, taskbar thumbnails, tray overflow, context menus, or taskbar-adjacent app flyouts are visible.
- Add a default-enabled "Freeze colors while taskbar previews and popups are open" setting.
- Exclude Windows' always-present invisible handwriting canvas from popup detection.
- Version the local bridge activation contract so this release can attach alongside an older dormant bridge without requiring an immediate Explorer restart.
- Add exact shared-surface pixel and popup-classification regression coverage.

## 0.6 local preview

- Add monitor-aware empty-desktop detection: the adaptive surface stays visible whenever a normal application is visible on that monitor and hides when only the desktop remains.
- Exclude Start, Search, Quick Settings, shell flyouts, wallpaper hosts, minimized windows, and windows on other virtual desktops from application detection.
- Add a default-enabled "Fully transparent when only the desktop is visible" setting.
- Record the current adaptive/desktop surface state and visible application count in local diagnostics.
- Add regression tests for application-versus-shell classification and per-monitor intersection.

## 0.5 local preview

- Keep the adaptive color surface in the taskbar's topmost shell layer so Start, Search, Quick Settings, and notification flyouts cannot reveal the desktop wallpaper underneath it.
- Replace run-by-run text detection with an edge-preserving median background filter that removes mixed and antialiased glyph strokes.
- Reduce near-edge weighting in background-only mode so captions and icons cannot outweigh the larger background area in the scan zone.
- Protect validated background boundaries before filtering, preserving exact split-screen transitions.
- Add regression coverage for dense mixed-shade foreground details on both light and dark panes.

## 0.4 local preview

- Prevent Start, Search, Quick Settings, notification, and other shell flyouts from restoring the wallpaper-tinted native taskbar brush.
- Add background-only sampling, enabled by default, to reject narrow text, icon, and divider streaks.
- Add an adjustable foreground-detail rejection width while preserving true split-screen boundaries.
- Replace the default Win32 settings appearance with a custom Windows 11-style dark interface, rounded controls, custom tabs, and accent sliders.
- Add regression coverage for full-height narrow foreground strokes.

## 0.3 local preview

- Replace the Windhawk runtime requirement with a focused local XAML bridge.
- Add adaptive light/dark system-tray foregrounds.
- Add the settings popup, tray lifecycle, multi-monitor sampling controls, presets, startup options, and recovery diagnostics.
