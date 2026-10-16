# TaskMon

A lightweight Windows taskbar widget that shows live **upload/download speed, CPU, RAM, GPU and disk activity**. It docks inside the taskbar itself, not as a floating window. The whole thing is one C++ file with no runtime dependencies beyond Windows.

```
↑ 123 Kbps   CPU   14%   GPU   3%
↓ 133 Kbps   RAM   88%   DISK  8%
```

## Features

- **Docks inside the taskbar** on Windows 10 and 11. On Windows 11 it uses UI Automation to find where the app icons are and sits in a free gap instead of covering them.
- **Network speed in Mbps** (the unit speed tests use) or MB/s. It counts physical adapters only, so filter drivers and virtual adapters aren't counted twice.
- **CPU** from the same counter Task Manager uses, plus **RAM**, **GPU** (busiest engine, like Task Manager) and **disk active time**.
- **Hover for details:** adapter name and link rate, today's upload and download totals (kept across restarts), and memory in GB.
- **Right-click menu:** position (left of the taskbar or next to the tray), speed units, show/hide GPU and disk, start with Windows, reset today's traffic, open Task Manager, exit.
- Follows the light or dark taskbar theme, works at any display scaling, and reattaches by itself if Explorer restarts.
- About 260 KB on disk and about 23 MB of RAM.

## Install

Download `TaskMon.exe` from [Releases](../../releases) and run it. Right-click the widget and tick **Start with Windows** to launch it at sign-in.

## Build

With MSYS2 UCRT64 g++:

```
g++ -O2 -s -std=c++17 -municode -mwindows -static taskmon.cpp -o TaskMon.exe -liphlpapi -lpdh -lole32 -loleaut32
```

With MSVC (Developer Command Prompt):

```
cl /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE taskmon.cpp /link /SUBSYSTEM:WINDOWS
```

## How it works

The docking and stats techniques come from reverse engineering the open-source [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) V1.86 binary with [REA](https://github.com/morluto/rea) and Ghidra:

| Area | TrafficMonitor | TaskMon |
|---|---|---|
| Docking | `FindWindow("Shell_TrayWnd")`, then a child window inside Explorer's taskbar. Windows 10: shrinks `MSTaskSwWClass` inside `ReBarWindow32`. Windows 11: detects `DesktopWindowContentBridge` and positions relative to `TrayNotifyWnd`. | Same, plus UI Automation on Windows 11 to avoid covering the XAML task buttons. |
| Network | `GetIfTable` (32-bit counters that wrap at 4 GiB), delta × 1000 / elapsed ms | `GetIfTable2` (64-bit counters), physical, up, non-filter adapters only |
| CPU | Performance counter `% Processor Utility`, falling back to `GetSystemTimes` | Same |
| RAM | `GlobalMemoryStatusEx` | Same |
| GPU | Performance counter wildcard read with `PdhGetFormattedCounterArrayW` | `\GPU Engine(*)\Utilization Percentage`, summed per engine, busiest engine shown |
| Rendering | `UpdateLayeredWindow` family | Per-pixel alpha via `UpdateLayeredWindow` (text blends into the taskbar and the whole widget responds to the mouse) |

Settings are stored in `HKCU\Software\TaskMon`. The start-up entry is `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\TaskMon`.
