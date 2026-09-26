# Third-party notices

## Windows 11 Taskbar Styler

`bridge/xaml_bridge.cpp` uses a focused, substantially reduced adaptation of the XAML diagnostics attachment approach from **Windows 11 Taskbar Styler**, authored by m417z and distributed in the ramensoftware Windhawk mods repository under GPL-3.0.

Original source:

https://github.com/ramensoftware/windhawk-mods/blob/main/mods/windows-11-taskbar-styler.wh.cpp

Project repository:

https://github.com/ramensoftware/windhawk-mods

The retained behavior is limited to attaching a XAML diagnostics TAP, identifying the two native taskbar background shapes, and safely tracking taskbar XAML elements. ChameleonBar adds its own local rendering, spatial color sampling, settings UI, lifecycle fail-safe, and adaptive foreground behavior.

The complete license is in `LICENSE.txt`.
