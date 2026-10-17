# TaskMon

A lightweight Windows taskbar widget that shows live **network speed, CPU and RAM**, with an optional **running character** that speeds up as your PC gets busier. It docks inside the taskbar itself, not as a floating window. The whole program is one C++ file with no runtime dependencies beyond Windows.

```
 🐈   ⤒ 30.5 Kbps   ▣  6%
      ⤓ 17.8 Kbps   ▥ 88%
```

## Features

- **Four styles** (right-click → Style):
  - **Subtle** (default): small tinted icons, values, and a faint scrolling graph under each value.
  - **Cards:** rounded translucent pills with an icon, a bold value and a live area graph.
  - **Rings:** circular gauges with the icon inside the ring.
  - **Classic:** the compact two-row text grid TrafficMonitor uses in its taskbar mode.
- **Running character** (right-click → Character), inspired by [RunCat](https://github.com/runcat-dev/RunCat365):
  - Built in: **cat**, **parrot** and **horse**, recolored to match the taskbar theme.
  - Included GIFs: **fox**, **dog**, **wolf** and **black cat** (CC0 pixel art).
  - **Runs faster with** CPU usage, RAM usage, or network speed. You choose which.
  - Add your own: drop any `.gif` into the `characters` folder next to `TaskMon.exe` and it appears in the menu, or use **Choose a GIF...**. Small pixel art is scaled by whole numbers so it stays crisp.
- **Animation:** graphs scroll smoothly, numbers ease to new values, and a card pulses red when a resource hits 90%. Costs about 1.5–3% of one CPU core.
- **Background image:** put any picture faintly behind the widget, at 10–50% opacity.
- **Network speed in Mbps** (what speed tests report) or MB/s. Only physical adapters are counted, so filter drivers and virtual adapters aren't counted twice.
- **Optional GPU and disk activity:** GPU is the busiest engine, the way Task Manager reports it.
- **Hover for details:** adapter name and link rate, today's upload and download totals (kept across restarts), and memory in GB.
- **Docks inside the taskbar** on Windows 10 and 11. On Windows 11 it uses UI Automation to find the app icons and sits in a free gap instead of covering them. Position is left of the taskbar or next to the tray.
- Follows the light or dark taskbar theme, works at any display scaling, reattaches by itself if Explorer restarts, and can start with Windows.

## Install

Download `TaskMon-vX.Y.Z.zip` from [Releases](../../releases), extract it anywhere, and run `TaskMon.exe`. The zip includes the `characters` folder. Right-click the widget and tick **Start with Windows** to launch it at sign-in.

## Build

With MSYS2 UCRT64 g++:

```
g++ -O2 -s -std=c++17 -municode -mwindows -static taskmon.cpp -o TaskMon.exe -liphlpapi -lpdh -lole32 -loleaut32 -lgdiplus -lcomdlg32 -lshlwapi
```

With MSVC (Developer Command Prompt):

```
cl /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE taskmon.cpp /link /SUBSYSTEM:WINDOWS
```

Keep the `characters` folder next to the exe to get the bundled GIFs.

## How it works

The docking and stats techniques come from reverse engineering the open-source [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) V1.86 binary with [REA](https://github.com/morluto/rea) and Ghidra:

| Area | TrafficMonitor | TaskMon |
|---|---|---|
| Docking | `FindWindow("Shell_TrayWnd")`, then a child window inside Explorer's taskbar. Windows 10: shrinks `MSTaskSwWClass` inside `ReBarWindow32`. Windows 11: detects `DesktopWindowContentBridge` and positions relative to `TrayNotifyWnd`. | Same, plus UI Automation on Windows 11 to avoid covering the XAML task buttons. |
| Network | `GetIfTable` (32-bit counters that wrap at 4 GiB), delta × 1000 / elapsed ms | `GetIfTable2` (64-bit counters), physical, up, non-filter adapters only |
| CPU | Performance counter `% Processor Utility`, falling back to `GetSystemTimes` | Same |
| RAM | `GlobalMemoryStatusEx` | Same |
| GPU | Performance counter wildcard read with `PdhGetFormattedCounterArrayW` | `\GPU Engine(*)\Utilization Percentage`, summed per engine, busiest engine shown |
| Rendering | `UpdateLayeredWindow` family | GDI+ draws into a premultiplied 32-bit DIB that `UpdateLayeredWindow` pushes to the taskbar, so curves, gradients, icons and text all have smooth edges and real transparency |

The character's pacing follows RunCat's formula: frame time = 500 ms ÷ max(1, load ÷ 5 × rate).

Settings are stored in `HKCU\Software\TaskMon`. The start-up entry is `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\TaskMon`.

## Credits

- Cat, parrot and horse frames: [RunCat 365](https://github.com/runcat-dev/RunCat365), © 2025 Takuto Nakamura, Apache License 2.0. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Fox, dog, wolf and black-cat GIFs: CC0 sprites from OpenGameArt. See [characters/CREDITS.md](characters/CREDITS.md).
- Icons: Segoe Fluent Icons, which ships with Windows 11 and is not bundled.
