// taskmon.cpp - single-file taskbar widget: live network speed, CPU, RAM, GPU and disk activity.
//
// Docking and stats techniques mirror what REA/Ghidra recovered from TrafficMonitor V1.86:
//   * Shell docking  : FindWindow("Shell_TrayWnd") -> child lookup -> child window parented into
//                      Explorer's taskbar. Win10: inside ReBarWindow32, shrinking MSTaskSwWClass to
//                      make room. Win11 (DesktopWindowContentBridge present): child of Shell_TrayWnd.
//                      Re-applied every tick because Explorer re-lays out its children constantly.
//   * Network stats  : interface table polled once per second, octet deltas / elapsed time.
//                      TrafficMonitor uses GetIfTable (32-bit counters, wrap at 4 GiB); this uses
//                      GetIfTable2 (64-bit) and sums only physical, non-filter, up interfaces.
//   * CPU            : PDH "\Processor Information(_Total)\% Processor Utility" (matches Task
//                      Manager), falling back to GetSystemTimes deltas: (busy) * 100 / (kernel+user).
//   * RAM            : GlobalMemoryStatusEx, (total - avail) / total.
//   * GPU            : PDH wildcard "\GPU Engine(*)\Utilization Percentage" read with
//                      PdhGetFormattedCounterArrayW (as TrafficMonitor does); per-engine sums, max.
//   * Disk           : PDH "\PhysicalDisk(_Total)\% Idle Time", shown as 100 - idle.
//
// Win11 task buttons are XAML, not HWNDs, so the task list cannot be shrunk like on Win10. A
// background thread asks UI Automation where the taskbar buttons are, and the widget takes a free
// gap (leftmost or next to the tray, per setting) instead of drawing over icons.
//
// Rendering is GDI+ into a premultiplied 32-bit DIB pushed with UpdateLayeredWindow, so curves,
// gradients, icons and text are anti-aliased with real per-pixel alpha. Three styles:
//   * Cards   - rounded translucent pills (Waybar-style) with a Segoe Fluent icon, an eased value
//               and a live sparkline/area graph behind it; network shows download area + upload line.
//   * Rings   - Conky-style circular gauges with the icon inside the ring.
//   * Classic - the compact two-row text grid TrafficMonitor uses in its taskbar mode.
//   * Subtle  - (default) no chrome: tinted icons, values, and faint sparklines under each value.
// Optional: a user-chosen background image (cover-cropped, adjustable opacity) and animation
// (graphs scroll continuously, numbers ease, cards pulse when a resource is at 90%+).
// Character: a RunCat-style runner at the left edge whose frame rate follows CPU load, using the
// cat frames from RunCat 365 (Copyright 2025 Takuto Nakamura, Apache License 2.0,
// https://github.com/runcat-dev/RunCat365), or any animated GIF the user picks.
//
// Settings live in HKCU\Software\TaskMon; "Start with Windows" uses HKCU\...\Run.
//
// Build (MSYS2 UCRT64 g++):
//   g++ -O2 -s -std=c++17 -municode -mwindows -static taskmon.cpp -o TaskMon.exe -liphlpapi -lpdh -lole32 -loleaut32 -lgdiplus -lcomdlg32 -lshlwapi
// Build (MSVC Developer Prompt):
//   cl /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE taskmon.cpp /link /SUBSYSTEM:WINDOWS
//
// Right-click the widget for the menu; double-click opens Task Manager; hover for details.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
#include <shellapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <ole2.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <uiautomation.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
// GDI+ headers expect min/max, which NOMINMAX (needed for std::min/max) removes.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
namespace G = Gdiplus;

#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")
#endif

static const wchar_t kCtrlClass[]   = L"TaskMonController";
static const wchar_t kWidgetClass[] = L"TaskMonWidget";
static const wchar_t kRegKey[]      = L"Software\\TaskMon";
static const wchar_t kRunKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[]    = L"TaskMon";
static const UINT_PTR kTimerId = 1;
static const UINT_PTR kFrameTimerId = 3;
static const UINT kFrameMs = 50;            // 20 fps while animating

enum : UINT {
    kIdExit = 100, kIdTaskMgr, kIdPosLeft, kIdPosRight, kIdBits, kIdBytes, kIdExtras, kIdStartup, kIdResetToday,
    kIdStyleCards, kIdStyleRings, kIdStyleClassic, kIdAnimate, kIdBgChoose, kIdBgRemove,
    kIdOpacity10, kIdOpacity20, kIdOpacity35, kIdOpacity50, kIdStyleSubtle,
    kIdCharNone, kIdCharCat, kIdCharGif, kIdCharParrot, kIdCharHorse, kIdSrcCpu, kIdSrcRam, kIdSrcNet,
    kIdCharFolder,
    kIdCharLibBase = 1000,                      // + index into the characters\*.gif list
};

enum CharacterMode : DWORD { kCharNone = 0, kCharCat = 1, kCharGif = 2, kCharParrot = 3, kCharHorse = 4 };
enum SpeedSource : DWORD { kSrcCpu = 0, kSrcRam = 1, kSrcNet = 2 };

enum Style : DWORD { kStyleCards = 0, kStyleRings = 1, kStyleClassic = 2, kStyleSubtle = 3 };

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
struct Settings {
    DWORD posRight = 0;     // 0 = far left of the taskbar, 1 = next to the tray
    DWORD bits = 1;         // 1 = Mbps (what speed tests/ISPs quote), 0 = MB/s
    DWORD extras = 0;       // show GPU + disk
    DWORD style = kStyleSubtle;
    DWORD animate = 1;
    DWORD bgOpacity = 20;   // percent
    std::wstring bgImage;   // empty = none
    DWORD character = kCharCat;
    std::wstring characterGif;
    DWORD speedSource = kSrcCpu;    // what makes the character run faster
};
static Settings g_set;

static DWORD RegReadDword(const wchar_t* name, DWORD def) {
    DWORD v = 0, cb = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS ? v : def;
}
static ULONG64 RegReadQword(const wchar_t* name) {
    ULONG64 v = 0;
    DWORD cb = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_QWORD, nullptr, &v, &cb) == ERROR_SUCCESS ? v : 0;
}
static void RegWrite(const wchar_t* name, DWORD type, const void* data, DWORD cb) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, type, (const BYTE*)data, cb);
        RegCloseKey(k);
    }
}
static std::wstring RegReadString(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD cb = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS ? buf : L"";
}
static void LoadSettings() {
    g_set.posRight = RegReadDword(L"PositionRight", 0);
    g_set.bits = RegReadDword(L"SpeedInBits", 1);
    g_set.extras = RegReadDword(L"ShowGpuDisk", 0);
    g_set.character = std::min<DWORD>(RegReadDword(L"Character", kCharCat), kCharHorse);
    g_set.characterGif = RegReadString(L"CharacterGif");
    g_set.speedSource = std::min<DWORD>(RegReadDword(L"SpeedSource", kSrcCpu), kSrcNet);
    g_set.style = std::min<DWORD>(RegReadDword(L"Style", kStyleSubtle), kStyleSubtle);
    g_set.animate = RegReadDword(L"Animate", 1);
    g_set.bgOpacity = std::min<DWORD>(RegReadDword(L"BackgroundOpacity", 20), 100);
    g_set.bgImage = RegReadString(L"BackgroundImage");
}
static void SaveSettings() {
    RegWrite(L"PositionRight", REG_DWORD, &g_set.posRight, sizeof(DWORD));
    RegWrite(L"SpeedInBits", REG_DWORD, &g_set.bits, sizeof(DWORD));
    RegWrite(L"ShowGpuDisk", REG_DWORD, &g_set.extras, sizeof(DWORD));
    RegWrite(L"Style", REG_DWORD, &g_set.style, sizeof(DWORD));
    RegWrite(L"Animate", REG_DWORD, &g_set.animate, sizeof(DWORD));
    RegWrite(L"BackgroundOpacity", REG_DWORD, &g_set.bgOpacity, sizeof(DWORD));
    RegWrite(L"BackgroundImage", REG_SZ, g_set.bgImage.c_str(), DWORD((g_set.bgImage.size() + 1) * sizeof(wchar_t)));
    RegWrite(L"Character", REG_DWORD, &g_set.character, sizeof(DWORD));
    RegWrite(L"CharacterGif", REG_SZ, g_set.characterGif.c_str(), DWORD((g_set.characterGif.size() + 1) * sizeof(wchar_t)));
    RegWrite(L"SpeedSource", REG_DWORD, &g_set.speedSource, sizeof(DWORD));
}

static bool StartupEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD cb = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS;
}
static void SetStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)cmd, DWORD((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------------------------
struct Stats {
    double upBps = 0, downBps = 0;          // bytes per second
    int cpu = 0, ram = 0, gpu = -1, disk = -1;
    ULONG64 ramUsed = 0, ramTotal = 0;
    std::wstring adapter;
    ULONG64 linkBps = 0;                    // bits per second
};

class NetSampler {
    ULONG64 prevIn_ = 0, prevOut_ = 0;
    LARGE_INTEGER prevT_{}, freq_{};
    bool primed_ = false;

    static bool Wanted(const MIB_IF_ROW2& r) {
        return r.InterfaceAndOperStatusFlags.HardwareInterface &&
               !r.InterfaceAndOperStatusFlags.FilterInterface &&
               r.OperStatus == IfOperStatusUp &&
               r.Type != IF_TYPE_SOFTWARE_LOOPBACK;
    }

public:
    ULONG64 todayIn = 0, todayOut = 0;

    NetSampler() { QueryPerformanceFrequency(&freq_); }

    void Sample(Stats& s) {
        PMIB_IF_TABLE2 table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR) { s.upBps = s.downBps = 0; return; }
        ULONG64 in = 0, out = 0, busiest = 0;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& r = table->Table[i];
            if (!Wanted(r)) continue;
            in += r.InOctets;
            out += r.OutOctets;
            if (r.InOctets + r.OutOctets >= busiest) {      // name the adapter carrying the most
                busiest = r.InOctets + r.OutOctets;
                s.adapter = r.Description;
                s.linkBps = r.ReceiveLinkSpeed;
            }
        }
        FreeMibTable(table);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s.upBps = s.downBps = 0;
        if (primed_) {
            double secs = double(now.QuadPart - prevT_.QuadPart) / double(freq_.QuadPart);
            // Same guard as TrafficMonitor: only trust the delta when counters moved forward
            // (an adapter going down/up resets its counters).
            if (secs > 0.05 && in >= prevIn_ && out >= prevOut_) {
                s.downBps = double(in - prevIn_) / secs;
                s.upBps   = double(out - prevOut_) / secs;
                todayIn  += in - prevIn_;
                todayOut += out - prevOut_;
            }
        }
        prevIn_ = in; prevOut_ = out; prevT_ = now; primed_ = true;
    }
};

class CpuSampler {
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    ULONGLONG prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;

    static ULONGLONG U64(const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

    int FromSystemTimes() {
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
        ULONGLONG i = U64(idle), k = U64(kernel), u = U64(user);
        ULONGLONG di = i - prevIdle_, dk = k - prevKernel_, du = u - prevUser_;
        prevIdle_ = i; prevKernel_ = k; prevUser_ = u;
        ULONGLONG total = dk + du;              // kernel time already includes idle time
        if (total == 0) return 0;
        return int(((total - di) * 100) / total);
    }

public:
    CpuSampler() {
        FromSystemTimes();                      // prime the fallback baseline
        if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS) {
            // English name works on every UI language; Utility matches Task Manager on Win8+.
            if (PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Utility", 0, &counter_) != ERROR_SUCCESS &&
                PdhAddEnglishCounterW(query_, L"\\Processor(_Total)\\% Processor Time", 0, &counter_) != ERROR_SUCCESS) {
                PdhCloseQuery(query_);
                query_ = nullptr;
            } else {
                PdhCollectQueryData(query_);
            }
        }
    }
    ~CpuSampler() { if (query_) PdhCloseQuery(query_); }

    int Sample() {
        int sys = FromSystemTimes();            // keep the baseline current either way
        if (query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE v;
            if (PdhGetFormattedCounterValue(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS &&
                v.CStatus == ERROR_SUCCESS) {
                double d = v.doubleValue;
                return d < 0 ? 0 : d > 100 ? 100 : int(d + 0.5);
            }
        }
        return sys;
    }
};

// GPU (wildcard engine counters) and disk (idle time) share one PDH query.
class GpuDiskSampler {
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER gpu_ = nullptr, diskIdle_ = nullptr;
    std::vector<BYTE> buf_;

public:
    GpuDiskSampler() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) { query_ = nullptr; return; }
        if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu_) != ERROR_SUCCESS) gpu_ = nullptr;
        if (PdhAddEnglishCounterW(query_, L"\\PhysicalDisk(_Total)\\% Idle Time", 0, &diskIdle_) != ERROR_SUCCESS) diskIdle_ = nullptr;
        PdhCollectQueryData(query_);
    }
    ~GpuDiskSampler() { if (query_) PdhCloseQuery(query_); }

    void Sample(int& gpu, int& disk) {
        gpu = disk = -1;
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return;

        if (diskIdle_) {
            PDH_FMT_COUNTERVALUE v;
            if (PdhGetFormattedCounterValue(diskIdle_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS &&
                v.CStatus == ERROR_SUCCESS) {
                double busy = 100.0 - v.doubleValue;
                disk = busy < 0 ? 0 : busy > 100 ? 100 : int(busy + 0.5);
            }
        }
        if (gpu_) {
            DWORD size = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(gpu_, PDH_FMT_DOUBLE, &size, &count, nullptr) == (PDH_STATUS)PDH_MORE_DATA) {
                buf_.resize(size);
                auto items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf_.data();
                if (PdhGetFormattedCounterArrayW(gpu_, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                    // Instance: pid_<n>_luid_<a>_<b>_phys_<p>_eng_<e>_engtype_<type>.
                    // Task Manager's figure: sum each engine across processes, then take the max.
                    std::map<std::wstring, double> perEngine;
                    for (DWORD i = 0; i < count; ++i) {
                        const wchar_t* name = items[i].szName;
                        const wchar_t* luid = wcsstr(name, L"luid_");
                        if (items[i].FmtValue.CStatus == ERROR_SUCCESS)
                            perEngine[luid ? luid : name] += items[i].FmtValue.doubleValue;
                    }
                    double mx = 0;
                    for (auto& kv : perEngine) mx = std::max(mx, kv.second);
                    gpu = mx > 100 ? 100 : int(mx + 0.5);
                }
            } else {
                gpu = 0;                        // counter exists but no engine instances right now
            }
        }
    }
};

static void SampleRam(Stats& s) {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m) || m.ullTotalPhys == 0) { s.ram = 0; return; }
    s.ramTotal = m.ullTotalPhys;
    s.ramUsed = m.ullTotalPhys - m.ullAvailPhys;
    s.ram = int((s.ramUsed * 100 + m.ullTotalPhys / 2) / m.ullTotalPhys);
}

// ---------------------------------------------------------------------------------------------
// Taskbar docking
// ---------------------------------------------------------------------------------------------
struct Dock {
    HWND tray = nullptr;      // Shell_TrayWnd
    HWND parent = nullptr;    // window we are a child of
    HWND taskList = nullptr;  // Win10: MSTaskSwWClass we shrink
    bool win11 = false;
    RECT taskListOrig{};      // Win10: task list rect (parent coords) before we moved it
    bool shrunk = false;
};

static void Render();

static Dock g_dock;
static HWND g_ctrl = nullptr, g_widget = nullptr, g_tip = nullptr;
static Stats g_stats;
static HFONT g_font = nullptr;
static int g_fontDpi = 0, g_fontPx = 0;
static bool g_light = false;
static UINT g_msgTaskbarCreated = 0;

// ---- Win11: occupied taskbar button rects, refreshed by a UI Automation worker thread --------
static CRITICAL_SECTION g_btnLock;
static std::vector<RECT> g_buttons;     // screen coords
static volatile LONG g_quit = 0;

static const GUID kClsidCUIAutomation = { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
static const GUID kIidIUIAutomation   = { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };

static DWORD WINAPI ButtonScanThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(kClsidCUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidIUIAutomation, (void**)&uia);
    IUIAutomationCondition* isButton = nullptr;
    if (uia) {
        VARIANT v; VariantInit(&v);
        v.vt = VT_I4; v.lVal = UIA_ButtonControlTypeId;
        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &isButton);
    }
    while (!g_quit && uia && isButton) {
        std::vector<RECT> found;
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        IUIAutomationElement* root = nullptr;
        if (tray && SUCCEEDED(uia->ElementFromHandle(tray, &root)) && root) {
            IUIAutomationElementArray* arr = nullptr;
            if (SUCCEEDED(root->FindAll(TreeScope_Descendants, isButton, &arr)) && arr) {
                int n = 0;
                arr->get_Length(&n);
                for (int i = 0; i < n; ++i) {
                    IUIAutomationElement* e = nullptr;
                    if (FAILED(arr->GetElement(i, &e)) || !e) continue;
                    RECT r{};
                    if (SUCCEEDED(e->get_CurrentBoundingRectangle(&r)) && r.right > r.left && r.bottom > r.top)
                        found.push_back(r);
                    e->Release();
                }
                arr->Release();
            }
            root->Release();
        }
        EnterCriticalSection(&g_btnLock);
        g_buttons.swap(found);
        LeaveCriticalSection(&g_btnLock);
        for (int i = 0; i < 15 && !g_quit; ++i) Sleep(100);   // ~1.5 s cadence
    }
    if (isButton) isButton->Release();
    if (uia) uia->Release();
    CoUninitialize();
    return 0;
}

// Finds an x in [0, limit) where a span of width w fits without touching any button.
// preferLeft picks the leftmost such gap, otherwise the rightmost. Returns -1 if none fits.
static int FindFreeX(std::vector<std::pair<int, int>> spans, int limit, int w, int margin, bool preferLeft) {
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<int, int>> merged;
    for (auto& s : spans) {
        if (s.second <= 0 || s.first >= limit) continue;
        if (!merged.empty() && s.first <= merged.back().second) merged.back().second = std::max(merged.back().second, s.second);
        else merged.push_back(s);
    }
    std::vector<std::pair<int, int>> gaps;
    int left = 0;
    for (auto& m : merged) { gaps.push_back({ left, m.first }); left = m.second; }
    gaps.push_back({ left, limit });
    if (!preferLeft) std::reverse(gaps.begin(), gaps.end());
    for (auto& g : gaps)
        if (g.second - g.first >= w + 2 * margin) return preferLeft ? g.first + margin : g.second - margin - w;
    return -1;
}

static bool FindDock(Dock& d) {
    d = Dock{};
    d.tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!d.tray) return false;
    // TrafficMonitor's Win11 test: the XAML bridge child only exists on the new taskbar.
    d.win11 = FindWindowExW(d.tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) != nullptr;
    if (!d.win11) {
        HWND rebar = FindWindowExW(d.tray, nullptr, L"ReBarWindow32", nullptr);
        if (!rebar) rebar = FindWindowExW(d.tray, nullptr, L"WorkerW", nullptr);
        HWND list = rebar ? FindWindowExW(rebar, nullptr, L"MSTaskSwWClass", nullptr) : nullptr;
        if (!list && rebar) list = FindWindowExW(rebar, nullptr, L"MSTaskListWClass", nullptr);
        if (rebar && list) { d.parent = rebar; d.taskList = list; return true; }
        d.win11 = true;                         // unknown layout: treat like Win11 (child of tray)
    }
    d.parent = d.tray;
    return true;
}

static int DpiOf(HWND h) {
    typedef UINT(WINAPI * Fn)(HWND);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && h) { UINT d = fn(h); if (d) return int(d); }
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d;
}

static bool ReadLightTheme() {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

// Palette, picked per light/dark taskbar.
static COLORREF TextColor()  { return g_light ? RGB(20, 20, 20) : RGB(245, 245, 245); }
static COLORREF TrackColor() { return g_light ? RGB(205, 205, 208) : RGB(70, 70, 74); }
enum Item { kUp, kDown, kCpu, kRam, kGpu, kDisk };
static COLORREF ItemColor(Item i) {
    static const COLORREF dark[]  = { RGB(255, 159, 67), RGB(46, 204, 113), RGB(96, 165, 250), RGB(192, 132, 252), RGB(244, 114, 182), RGB(250, 204, 21) };
    static const COLORREF light[] = { RGB(214, 104, 0),  RGB(22, 140, 70),  RGB(37, 99, 235),  RGB(126, 58, 200),  RGB(190, 40, 110),  RGB(160, 120, 0) };
    return g_light ? light[i] : dark[i];
}
static COLORREF LoadColor(int pct) {
    if (pct >= 90) return g_light ? RGB(200, 30, 30) : RGB(248, 113, 113);
    return TextColor();
}

static void EnsureFont(int dpi, int rowPx) {
    int px = MulDiv(9, dpi, 72);                // 9pt
    if (px > rowPx) px = rowPx;
    if (g_font && dpi == g_fontDpi && px == g_fontPx) return;
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontDpi = dpi; g_fontPx = px;
}

static int TextW(HDC dc, const wchar_t* s) {
    SIZE sz{};
    GetTextExtentPoint32W(dc, s, int(wcslen(s)), &sz);
    return sz.cx;
}

struct Layout { int colNet, colPct, gap, pad, rowH, label; };

static Layout Measure(int dpi) {
    Layout L{};
    L.rowH = MulDiv(17, dpi, 96);
    EnsureFont(dpi, L.rowH - MulDiv(3, dpi, 96));
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, g_font);
    L.label = TextW(dc, L"↓ ");
    L.colNet = L.label + std::max(TextW(dc, L"888.8 Mbps"), TextW(dc, L"888.8 KB/s"));
    L.colPct = TextW(dc, L"DISK ") + TextW(dc, L"100%");
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    L.gap = MulDiv(10, dpi, 96);
    L.pad = MulDiv(6, dpi, 96);
    return L;
}

// ---- Character (RunCat-style runner or a user GIF) -----------------------------------------
// RunCat 365 cat frames (32x32 PNG silhouettes), Copyright 2025 Takuto Nakamura,
// Apache License 2.0 - https://github.com/runcat-dev/RunCat365/tree/main/RunCat365/resources/runners/cat
static const char* const kCatFrames[] = {
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MDI6MjUAEs8rIgAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzowNTozOSswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MDI6MjUrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgoJx3wYAAACHElEQVRYCe3Vy0tUURzAcXvYCwIRpe1ET0oKInEjUptIo3atXQhF"
    "9Ce0aJcrXVeLIHARVJAyUhCJVBiKRIoYLoKc6EHR20dZWd/v1BlmYG7dsRnc3B98nHPvOfecc3/3nGNVVRJJBpIMLHMGVlVo/E30"
    "ewjrkcIZrMNjFMTqgqvyXKyhm9PowGVsxH7cRzW+IReVmMABev+MPrTiO2bh4Ga8ohNwkCb41vXYDe8ZNXiPNH4gG+VeA0fo9QSO"
    "Yyvu4BXMSC+c2EpMIxtelCt802Y0/Okww+8l9GAUrgGzshO5WJErxS846T04im34ijnsQCNqYfzEBFz9W2C7WbgjLiJ2OGAddqEF"
    "5/EWDlCqGzyzD7FiL6268QSu3FIHy2/vGvBzhM9DMTo2U9WJ57CTF7iFe/iA/I7jlId45jAit/xJKq/jJkbwDm4Ty+0I39TyFOIM"
    "mt/Gb+6JWDSclaeUM1yLp3Ay1/AAnxDiGYXX2B5uxPg19W8w/6+2rlT9bVumqL+C/LcLZVf4GAbxEuG+u+AYIiMM+IUWWoxs+bt+"
    "oUi9987BHXIQFzAD4y4eZUsRf8IEIqoLbptG39RwomGyfiZPto8wNsBsmpF+ZBAZkSuzyBMO4M7w7QYwiTZ4KLkuXKwuYOsfohu3"
    "UdZwwGG4HU/Bo/cquuD2rXi4U87CkzANj97/ilL/G3o+uKg8+93b43B7JpFkIMnAkjPwCzOBpbl1SymNAAAAAElFTkSuQmCC",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MDI6MjUAEs8rIgAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzowNjoxNiswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MDI6MjUrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgqsh9a/AAACBklEQVRYCe3U3StkcRzH8fG4nmeFsMKF0lJIcqEUTdxJ2WySaC+l"
    "uKAtbqT8BeLOnXLjQlJ7tym3KCm7KI2HiVl5pmk9ZHl/JkcnNdNgz9w433rNnHNmfr/v73y/v3McDjvsCrz3CkRZXIAPzN+OIhyj"
    "G1/gwSEc0fqwML4y93csoxqF2MZH+CPSOLDgO485K6HkGWhFOm5wB39YWYE6MiQhC1pIClLxGVrEPjxWLSCHyV1og2IKbmhR0/Ch"
    "Cl4rWmBsvHoSGPGbgzns4hR/UQJnBB+vDScDdRdliIeqqV5XQLs+AUYoqfquNiiUdwJ9z1ug1RegGOpdMuJwjzPo8dFdKGknyhFK"
    "FdV7c2xx8hMXuhiLWoxDk2ulSmiFc+adhQsx8Jeii+9+6I738Aub+AMNuIJeWKpMI1Sd14SqN4RJXOMp1L9s6OUQbE/oRaKN9JbK"
    "aC8MQ/lCjkz+qX6v4B9CXcAJ/zU2n3nMLdfHkI+goc2lki/CPEGwYy1QLeyBWqrN1ws3zOPWOG9CwFArOrAO80DzsXbwDAahhC0o"
    "hfnx49QfeixH4MUP1EBPXMBo4JcFKOElNnDweK5rRxhAGl4SqkhKKAO0UXxYQjNUxh0o+Tz0Sn3+DuHS/wsnU2mnJkLPq8qn/q6i"
    "EWENvahGsY1v0HnYI5eMnxAV9sx2QrsCdgWsrsADOIKhtsIcWgcAAAAASUVORK5CYII=",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MDI6MjUAEs8rIgAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzowNjoyOCswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MDI6MjUrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgoR+DzJAAAB/UlEQVRYCe3USyhtURzH8eP9iIiBUAYMhEwoA0ShvK507xmYGCjd"
    "OROlTJQJA2YmygApIwyUEOlEEt2kpKtuHhPllev9iu+v9tbpxLYdxcD+16d19llr7//a/7X2crmccCrgVOC7VyDokwqQRZ4GhBn5"
    "3LQR2POdQDB/pqLQkEEbjnPcwJ/I5qZW1OAMVajDuiihGeX80MB8hJh/Gu0B7RT6sAS7k4lnbD12EYhmeLCGW7i8J3DK9RzmsYMT"
    "qEIpqECtYZC2B1sIQDQ0Yd8qqU/JixGLJGhsNZTrj3FN83YowQ8s4gEq3zBWcI1H7GMApYiCJqx+9Yn6fmMWk+hFhWb5nkhncAd+"
    "QSV9KZTsDur3rnAn16NohJbxGOam5Ke9SGBYP+5hvpnd9op7/kNrv4FNtMFWaA1/YgZmye0mfm3cNM8qgGWojNo0C/DnrX2Ta/+M"
    "oAR6KcvQ+jRhF74P+si1zoIh5MByD2rH/sNHklndu8qzK713KdfPoc9Ox6fOAE3Cg0gUIhEKnQM6E8agT1AV0+enZdNXcIFctCAf"
    "+l9xCH0F2k96xqsRQ08y9NBwdOESeqsJ5MFO6CW6ocS6V5u4HXGwHarGOLQRtX6ZeE8EMbgMU9AE/sIN602gAUaE0hZBS7OMI/gT"
    "WqY0aDm2oePbCacCTgWcCnxtBZ4ASRO5eUoOWZUAAAAASUVORK5CYII=",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MDI6MjUAEs8rIgAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzowNjozNiswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MDI6MjUrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgpYtRwaAAACOUlEQVRYCe2WzUtUURiH/Uoxq4UoGWoEbtqEFmpQ4KKlLYQQF4Hi"
    "okUtqlUrkTb9AxG2iIj+BBU3wZQQQS2EPjeFGyXNyD7IoEJLn8fmwmVmdO4RlMD7g8czc+57Pub3vudcS0pSpQ6kDux2B0oLGNBF"
    "3zlohq/wGh7CW1iGbdcNVvgIK7Ca5SftKJyCMtgR7WWVFuiBe/AB3sF5qIRQOd8BqAgdGMWbmkkwFX2Q1Ik92fgp2ttwFDqgFSrK"
    "+ZNUMwTOQSeczg46QnsS/FXWi2mLy/kvQjf4bBF09DJYf88gWEOMcLGoPmytkREwZZFcoB8ewV3QvT/wCjIwAEldJPSf9tHchN8Q"
    "34Cf/8IE6JCL98JLiOK+8PkbGKcTF6A0tCDM/VkoVIQu6jNz6ylqgoOgdOwafAbt17FGqA/ZQDsD3EDcZr7myYUlrhq+OF7n3Mwd"
    "OAw6mliXiJyByNKttivMYTGPQ3NIEehWyKkhvKCcw6Pp7boQkoIFBphDcxfJolL7odjmLLzHkAEXnwYLMrEOEXkfPErmcRjqwF9z"
    "Aq6Dk5umX5CbIi+wAaiGLSs6Wp+YYXCDWaIj+ILnuZtYpm8SzsB6+kNqgDHr13C8EO3LlYs+gCdgwcVlyr2Gj4HOBb8YZhnjS2ke"
    "rIeNtMSDMTgOvkGfwi3wYvJCsh5yN0dXMjUQ1gRVRcItyqvwHtzQFci7wIpVLmPy9IOe72AxbiZT8QaMawOdew6O33HVsqL/C6RK"
    "Hfj/HFgD3gmQ1IfGHggAAAAASUVORK5CYII=",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MDI6MjUAEs8rIgAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzowNjo0NSswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MDI6MjUrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgqdcq3bAAAB3UlEQVRYCe3VyytEYRjH8WOQW66lLFxKsSDFQlnZuGRjZWUrC0kR"
    "WUhJlvaUhYVZuJWF/AEWKCF7Cxl3Te53yeX7y8wY08yccaZhc576zLm95708533fMQw77AzYGbAz8M8ZiI9h+yXUPYBsHKEJmThB"
    "zKOMFpw4xDDG4EYvkuCLaDKQTC0p+MC7r0bDyOW8B08e7Rwd0MjXsA1LoUoqMYJN3EONK70aYTUyMAgX7vAKlZFLdOFHBrg2Db3Q"
    "gEXcwlth4PGNZ8/wb3SS625sYA9L0FzwRQJnGlUddrGMGyg0eRrRgRoo5eFCGQoc3RX3jnENJ86RikSoo4Y6UIBWVEGTRt8qDUXQ"
    "rFUo3ftQitWxSKPPr6DmhmL66/D9q4nYglVo4nhTe8b5AtpQiHqswPvcylGfQlkNGnHcTYcykofAdBZzbx6/bVgrRJkdhz632rEc"
    "zbypFRBJJx4pNwWtjsDBcMt6dPLqAcw6MUeZCrNmHGYFgjzX+tbozMJFgVOzQlY6oNGrYmUgXGjPeAhXIJpn6ngOhqDVos5sYQY7"
    "nmv9EZl+e+0DVkIzW1vrBLRx5WMW2nRGUYosaFW94E+jn9bcUDbKzVqOak2GqFy7aC30mdZxATvsDNgZCJmBTwj/kI5L/OARAAAA"
    "AElFTkSuQmCC",
};

// RunCat 365 parrot and horse frames, same copyright and license as the cat above.
static const char* const kParrotFrames[] = {
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNjoxNSswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgrpSuk5AAACYElEQVRYCe2VPYsUQRCGd3pmdsbdVREOThTEQzONFDExkAsOQYzE"
    "M1YD/4CYiGBobmAgYmomiBgJooGxiSDeiaAgCoIup4g3Nzs+tfY0fU3vub07Rm5BUdX19dbU9EerNaPZBP73CURNDiDP84WiKI5Q"
    "M4Y3er3e036//7VJDG+tOI6fCSBceXhdKXXVmzitEeBlapRJkizC1yzwEv0j/AEeNpYqdRm9OQL8HNXqL5bCEfQJuZGm6VExaDqI"
    "/AY3+rt3U7CA7Qakg3eM+yZ20V8hVuB5mr2UZdkB9J3w9ATIdarU4BVgD7Dd1rYTAF6w/PfRMz7/F7zW6XT2sJ6KFNnrsGnA0fey"
    "vmXZZBJCb2BpVn6TISkWSjLGNCCpp2P7IquqmkOY/RDcAOM9owt6BadhPzEvLOcPre/QEnd80fKHqYzwJxmjxl+xF67g3w4PdNwj"
    "ZALbeXdZT0QyupHg4qNB2fkS9xqWvbLAF59FmjxuzEXWE5MpRAWvDuBpfNuUat0RSVNfrNgC3ewB9GDyglLFtWe6sntkh5sxFDVX"
    "UbRKEiJ66wFzweVXfCZOTsAmX54kJ7EFkYA+JKNih5/a9ec2kzFuKuxbkzc8+46PZRjJ1WrAut3ufLvdPoTN/q/Gb8e6Og09wTY+"
    "8cU3iHaLf+exkTe/k3KeKfreE+PmDNfOI0Xa1nQYt7cQ9hLgx8iuVeLlFvF1HXnE/k50epyoOqlJOVYDS4CX/6IBvXdGT4AvP4Z3"
    "rB0+SYNc0/dcdLmjDZVleZ7FczHUd65xOgp7gIetCrrRBoPBPqfMbDmbQOs3iUYklE4r+qcAAAAASUVORK5CYII=",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNjozNSswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgodeCOcAAACdklEQVRYCe2Wv2tUQRDH9/28EHMklwSiWBhQ8AeKRYKFUUFBrK2E"
    "2PgHBHsbW/0frCy0UIiJjZ2IYCoDNiFaKVE0CipXqXf37vzMy+7j5d2+3OXlzsYbGL67M7PznZ2dd4lSA/nfO+DtsQH7OL8fHUVb"
    "aA39N+K67i2YPqNCLLqJ7S7YX6lUKqOOo5qwGOIsfsIX9KsKabXyff88kCb+wf4L2nQcZxMcQnsuh8loWlxhHRfged4N1rFQ2Dm6"
    "89Tse43r3O6tJC2VSkeAFmSPNclz8BU6xRzcRBeIfTKGaP/eINNyafdvtOX76gLFXJG11vtgiP6SPQWugT2RB2QxJGk8hl1Ije29"
    "ZtvQNhnWYW2zgmu1thvPtpsSi5AbKelFVaPDjFwyTht2LGByUpU5OG07jG0Mgmcpn9xYZGQLlIqi6LRZ27BjAdVqcNR2UGwM22UI"
    "FlP+d3qdDB8xh1L+QstrnDJvnMXv+Fwm/gMYBUEwy8BeTMcziMvsc8XP9WgHNzjZbJrOtkWPQzjXaDROyGdXr9dXifiYiZKvorjw"
    "xtc5nb15dm8usmCJfVGcfevkAaCOZkmTPbeXp5C/jH8scfIDVVhkSAOe4Q6YENrWvPVrmx3bV7SwTHmeE/+2u666R5Ydi8jzM5wz"
    "hSrg/eclKR14JAlIdIaR/ya23ShPtCTni4iMf0xGkp8Hy+UJnUSeRjTxm7g8DMPwFL6uRf5VW0dtN21gf4k+RLsugAtsEC9Fd5Rh"
    "gvPIbQV1bQtD72on9iHI3xDUddLdxPKlyOe8o9zG2xfyVN5tBZhfsNjItB/nZ3dlW0SPNzzDfK0WyfwMZNCBuAN/AVidHhQWozlJ"
    "AAAAAElFTkSuQmCC",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNjo0MCswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgopTogmAAACUElEQVRYCe2VO2sUURSA72N3zcwuOyQGjMIGRCEWIhZp/QF2FpIo"
    "BrGwFKwESwtR1MJaEMQikLWyimAjgrYiYpEmpLDTiBgivmf8zmZnubMzrOPsrBDYA2fO+zFn7j2j1BjGE9hVE7imTKffmP6P5n3f"
    "30+dl+BnMHLwO/xGpVI5AR0N1Gq1BaXVttZ6jQpu8W30T8BN0Ver1eOld0DxY5I8mA0mO8m1ei4yH2EFqsE6aI0xV2zNnoIvHUIy"
    "Rp7nzUpmCt0Wucvf6vKrUN063Dok+tLAWnuRZPHIH8EfBF/xKdahVccmPjM0dwl6B5TJlAJvyBI30KPGmofoJ/psc0EQSIORsuoC"
    "tBT4SJZeYYd/Ae85svgcAfd0dXIzpJni4E/7B4jOKh7rZMy/HJ+5xmTjqCN/hW+CheEMkXGxLMqJ7Hzv2DbFGbjnxiDfKFydQAmO"
    "k6coB/Qc9hb40xh1HSqQ9NPq9Y662PNBKmGygIzY48AtdqhSyyl/o9rocsHObk+6biTFlDTBZXuqfqt2A8AqjfTDl35Fbpm1Oo9z"
    "cqRZsjGX8ZNFlPJlX8g1HgK0ekd0KvE/6vzCHXT/cMM2IC8hW7MwDLwNZM3T4CcO6+kiHdyXIK7cScg3ME+xQT7PyCErPDe8x3Or"
    "Xq/vg1owAPeCsmoHFRpke0ts1q1D7QBvfRbRTbTJZruJ/jz6H3021++vPDmWnFKZrNzrD8MUyRGbKJwYCW96Fet0wqNkodlsTrkp"
    "5c/WA/737SiMZnqKETAsqcdhGN4dQepxyl06gT+xqRjHQK3P+gAAAABJRU5ErkJggg==",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNjo0OCswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgoGU1T8AAACfklEQVRYCe2Wv4vUUBDH8/Jjk/UUlMNfuMiqlaCFYCEIIggKiiio"
    "tahgJ9qK2GjlIbbCwWFpJ/4JWlnIWoocgp2onFcI5y575sXP5PKyyW523WTX6nZgMu+9mfnOvHmTvFjWjGYV2OwVUKMK0Gw2A3t+"
    "3t9hWeutVuv3KNup6hzHuQxgF476+A/ztud5t5H/ja6D3B94YK6U6mC3ddpZbAPwJ+zbtv0AaQKvOL5/vl6v76M6N1lfs1z3HDYv"
    "GU+P2NUH0L4kiC5SSi5JbIePwo/h/fAJ2LKC4ACJXYzHkz4ajUYdjBD+lmBJNUKS+oR04DU4Yv4RadVqtSNU4J6l1DuZT0y24zwH"
    "xJT8B+NVmRPkGXJLRic2e7BfNGvY3GU8MX0FwSSQSs78Buv9CRxOgqZ21tzc7ioZ2OJEczUQe4sAwjCU89YFOjmWHrXbV3qT8Udx"
    "At1u9/RQF6XMNyGbxCq9cCzno/XJ3HzMSZxAFEXS5cUURbtQaEr+KDFoIb9TmWvFDuVW4wT+5ULwJ1rrp+z6F7aX6AsJ7mf9sHmf"
    "nZcaA3gBh15DDY510icGVxLJ27vuWaOsIrlv4kbLg+aDyFvisdMF5IAdm7hVJXDWZ7kIOLvGEbxm3s6upWOl5NshX89qxA13HE/p"
    "9IHdjbtmu+79stHTd5kmkxLLDk6VBTH2kdZnGK/A1RuSM34IQOUq4Ks5qlfI8oSjXDQBt9xBLpnPjDtw5WOxPe8O/qX+GSRgh46+"
    "igxgQ5NURC60JYB2GrBRch3lRjCl5Pp9i/OLdM3oqknBOwTnKPdTqhznDVrzKkkiQjmbjaWhT/EZao9iWYehfC8M9lCgmWLzVOAv"
    "dM0EKDrWqeYAAAAASUVORK5CYII=",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNjo1NCswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgqpYYA5AAACVklEQVRYCe2Vz6tMcRTAv7/ud7w7WExjIz/KAukVGRIps5GlJRaS"
    "ha2yUSieUrLxF9hJiWzYUspSVigb2VgRCyHGfff6nHkzuvfO3Hlv7r1voebUme/5fc6c873nq9QMZh2YdWByB/Rk9SppG43GUUL/"
    "BmMwGaDQPa31u1artR56VcCS4AmRh0kLT2PM5borkHbfX0nyoY1zrgtdD1hrzxEp4Z/dUmrBaG2fCg++UUGwt91ur1POyWh+KaXf"
    "B0FwQFv7CL4WCIkSgQmFnBhEnOP8Dsq8j4OPwQ1K+XmKvA0tcCMMw41LZIVf5v4a9/68CX5NQjWbzT3IX0JKcT1Q9FKQxuat934n"
    "5Cv4b2Bl6CcnSuZ0zl9FtjsnVxRwMyPT+jl8KTBcpCNFnnEcfSnQfczIk+Rwhp+CMXzcx4rt9UF0Iy2O41juRxosjEkLVkqbOIp2"
    "FBnH8eJ+dB/Az2kbRjDOR8Y3NSxX9dZut+tI+FAicymfcTg6cDqXaRG+VAFyoRYGzhJgBLkk15ELSBfW8Jme5MzbyVdSDlgo+/DM"
    "B8zy3s8nSSKbUmad1S3xskMqPVzjgqZlcutD/v2DggJkg15EVw60NrJQ0glHaOYva/fPBLuf6LaDpWAtXiNJS8i+4rOlVAU4bSuR"
    "cFzRPUZ1ZqoiWLmX2O27cPK0+gVn/2GqWpD1Xh42eUuWBdlscpGucM51Op1AXkX4CyKvisS5S4xNYDEY8++G/8DhDgWc5TyPR+UC"
    "hjF4dw7lK0h/u5tp/72Bcd6uLj5in5wi2Ke6As7i/P8d+Au5kQ8cVeMt6gAAAABJRU5ErkJggg==",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNzowMCswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgpOOtC1AAACc0lEQVRYCe2Wz2sTURDH34/dNoHdIGoVaQ0q+Adoi/Yk/gOCnrwI"
    "UfAievOgiF68if+AIPgXSD2Lgr8OepEiqAd76VEQkSaatGazz8+U3Zik225+3ZqBybz5zryZ2XlvZ6PUhCYdmHRgt3dA5zTAx37c"
    "87zznvXKTukoiv4ut1qt5+Df4Shn/0jmM+z+Bbss1lpXPeWJz/jJWr9C1IYx5gnyG5xZhOBBEBxAjo+stReIJgnfSdRisTiHkFYL"
    "Vrd26qJS/ilj/BvoLXh8FIbhfqI1YUn2Gk7pNwsXhrP7kDMUeRkp92OsR8Cx6q8ETdsthSyQrCIYtlWkh2wk+huk0AbYC2Tehd50"
    "3vaHRJcwpsm3SGO8u9jLPT4ByZ8mmNwX6cpwRKDP7NySOMWm7fQ51kdSPZF7uag3U4wY71lPwX2TST2dc0fTdZZsuuYiuBxLF9G5"
    "egoQYzHpZArlynYBWukfO3o7dQX7zx6fJkPpZCfmWm6hU89b/y/A6Fc7OccuPoR9nZY/Fj/avYqoxXF8VfSRyShzjSDb3gGxMQPk"
    "oh6E5bUs+75/QvBO9o1/HX0oYsB0B8vQ64EKZsDnYHntar0+g94B9rfpMKuup8nW9TK45igeZNtVBXw44lwleG4RJH+IXzXL12r7"
    "crjs7CoUCvIqxlmBB8RK+A9GPNUjdoTIe8jcLvThc3qgCmj/ErPgE5tkkt2C5Us3UiE8jExJC+cTzjLLJeEXeA/HcYyinrFeT/Ch"
    "iiHGWz7ps8TIJHmVNom/XWeZavcTdaNULN1eq699nFfz/opaCWv6D8XEbf/Er18RhYXwTrVR/dDvhonf7unAPw6GEVgrNuKWAAAA"
    "AElFTkSuQmCC",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNzowNiswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgq/y8rOAAACdUlEQVRYCe2Wv4vUQBTHJ5OJmDs0CgdpDsFmqwNBEEGsdbVT1FrQ"
    "Rhv/BKtTQUQEuV4Lm5VDUPS4ThA5KxWRw84fhaJroZ5XaH74eSGJu0MSyHqHTb7w9v2Y92Py5k2ySnXoOtB1oOvAf+6AM0l913XP"
    "xnG8tYidUTO3hmr4o9A3i+/QWl9yHOcjBVKLItbuYAs2pbhRpk/ib1ZRexOif4W2QBsHT3vnyZYV4+mXXMd9WOhVHJ/Fjauu1Jmi"
    "CIlj5GJmktweYX9qtJmHD7EloQqn4f8OBu0YWew2u5KZtU+w2Df+Qbic+26IPTiLvu/PInuQhiZDGGZP8Z3osQ0waAsUPy52Rzkv"
    "4bKht6Kzdhm+DVpnI6/xO4U8GUh2hcix4rZO2y9i22fZpQ3PcttvuGyoEZVtStP0UGMUi652V2Ch7ceQPslthgE+ba/beuUGcOrZ"
    "jraeJImc+7ptj5JIji5DlEbXCrmO121A7nMj6NIRHJ5DclQlOL5y8/jIMLYHA/aYqMYZYP1DnvlV7rsGl2v6JdeLeNSW4CnmCSkS"
    "1HLPm96D32FI3hGznuftrYjD1BJBEOwk5BdUW1zWmPg3cM2VOwkXSBfKGDr5M7O2/JnDP6ALN0eT1cla6RuSH/8LFT7yUmoHEskX"
    "b5koGdAXUPlEdbKnvP2syb0vfclzHb01fNpWJLpKtIN+F14mbiHLa7odOMsTRJTF6MQ9dG2MOYB8Xzv6c36upc+o/6iM3yN0AzVC"
    "rk0J3m4LlJ9LVJLZOV/hw96u3rnVd6vyR0Swnc08EEHW8RXOr3wg/0JsYI2L+X7Kn1rpH+3fHgwGcls6dB0Y68AfvxXvdhintnoA"
    "AAAASUVORK5CYII=",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNzoxMSswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgqqpMzMAAACWklEQVRYCe2Uv4vUUBDH895L3mbNLueCnBbiLVoIauVhZSeIaKmo"
    "WGjjP+AViqCF/gM2/gV2IlcKglY2FiKKJ+IPFLFzT1SURTyyJn5mSfayS3Kb3b3ODExm5js/M3kvjlNRtYFqA9UGqg3MuIG6U9+p"
    "tV5SSj2j1Dv4LfYt69h96HrG8humG5ouExEXMf7H+OyGVaZxBkGwneLfihpncbZxbZoehTnSHOdPaeIZ74I19ozoKRttbtec2l7w"
    "02BrrnKfIjeNLG/+g2ppw35hMGkSW22vIo3rukebzea2Vr21y3f8hX7QJjzkmz+RRinLAUQHVisJVk+Hwe7CNd/3dyOFZyO+5SUq"
    "DJonekjDD6IrR31JOmRjjnnaWxK/53gHE/9UQk5ytnCe/iqpnPWdBzskuQz6MvEPiVL31PO8i0NZOQYN2jnwAIrj+ABGawAkSqkB"
    "oii6PJo4atOgASZn4HnGt8qnS1evuRknMr6J1JDo7Gpz9YZtyN/vcBL7Bym35kWayzDX0ScnrtVNsnKbZnGt9Cds2eqKMeastXY/"
    "ei+NATuOPhEtMvUdMnx47AASQ/wVpHwOuZ+vBUv4l2CT0ALB3+G43W77FL4hegnuzdXm9vC2J7OxXNP32KVpB9PLxP2G6J/RXaPM"
    "wxQbIzvkfB2JkbNQiraSHBE59LZs4C4YLvVo1FfGZgMfiRtLOq85WekmHqAHvusfQb6BZUu/4aFhC+w18C3wEKmsxT09F/4N5RAV"
    "k3K688H8qU63sypB/KQWwzC8X5yw7nG1e68X9dJf+rqj0v7rDfwDPNzlLxFkPu0AAAAASUVORK5CYII=",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNzoxNiswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgooaiy8AAACeElEQVRYCe2Wv2sUQRTHZ3Z2534kJxKvEStFBIWAYND2EARBELTx"
    "F9ilsEkTRK21ETSFYBrF/AmpBHslKtimUEQUTCMo+AMv510cP+9ys9yPvb3lcmCzD16+89583499OzsXpXLJJ5BPIJ/Af56AnnT9"
    "UlQ6UW/W54xSm1tKPSG/m3SNpHw2CIIFrfV6p6AUddg/8D8ql8t7k4LER6M7llkKvdBOG6fccbJZnzHQeoMR15vN5hKN/HHOrfm9"
    "iSBJF0nkwPuS0BhzXmzRogproBcbavPMG5NATdGHJGoXC1SwJElrtVooPp78Axign8VGT6ETEyn+gGzt4h38YpU9gv+22NbYC+Bh"
    "z+EVbbBW4KrgjoQiN0jQXXxgfXBmZhecY308ORtrU1F0FBxPwjCUUQ4U7PdNWTuLr7+BAs0va6Wfj1edKBLIKEc2wJm4Du9AH1cm"
    "8BLdKqrifnAsGVmcrPL9fwUB/Vts9D0q4uPvbZvZ/8rpVkabV4BPkoZ/4UXorQ5fDuSeztrxGiRPduFUX4K9m+/8HJhWON7j6dvf"
    "Pa/tplTC/tYV+1Z8mYXgdfQpAdNoA40Lpa1p+Ar7ckHNd/O4HeXKziY8wTWYMlJXrVYr3N3yjWdqAN6vklL7QDkTcQyHdAV7tND5"
    "RVj8qMXBcrPJRXSnyxcnHuIbmFiowpNwR8pZGEnJ3+CPaO4M+H0IJynO+z4Sky6MaAGGD0jCT3b7slGVQuWQUeYq/M0RMe08TO9x"
    "anUId7MkEg4H8zX8RRq4jN3znoflKIThafZi6fmPKIqiuVarJZeE93uU7kXElnUSyr4Xzxe7hztt7fzPRuOdJ+aYT+Af704SkUr/"
    "GVoAAAAASUVORK5CYII=",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjkgMDA6MDE6MTYAHfNMzQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOVQwMDowNzoyMyswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjlUMDA6MDE6MTYrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgovSBQNAAACfUlEQVRYCe2WPYgTURDH3+5mN25ckZynHhK0CYKcdypaaSGC1RUK"
    "dn70NmoryNV2goWcCHpio2AhFnaCKAhaWQjaHCg24hcieohkvV1/E/YlLyH7smcSRMjAMPPm4z/zZt97iVJjGk9gPIF/PAFnBPVd"
    "MKfhSQP7I/prYz0yte44zmPQ0272XHcRWwR30LAmELmuewPklTRNy/CxjirtxS/UbfCntmlwbTu7/gCM7FrIZf0W2ZwC3+N8SakD"
    "nlJHHOW8w75BgoZFMwD9gJvFDNAnYqPoCST11UV4HzwB+/BQSHYiI9XFU3Z7hfUcHGf2MtN4lukNpHyqm3z3++gDkYz5Owit4t06"
    "/qWsghkzFwXBTnzLurpcmVUTu7jFQVtnSwT4aw//puVG4xW5a7XvbxqYSpLklAbIk4lSm/N8Yg8876TNn+tj92dxmmO16WsY9yMj"
    "/qjv+3uz9fvcIjYHIztnANqKp2EYbiV2NouXQzjBBm7rfK7HIfTiNBVFG0ul0n4yrIW1n5P6QtCZwkvEmVqtFiJ/ws18mpHXsRgF"
    "QTBDwtUsulADxKae5x1HliWPRh6ITTPTvCP2QkRxueMyxhD9LrIF1EePOfJyIHd1x9HAJWz9yTg4KcUvk8Ejp77AhZrg4Vli9w+7"
    "4pNAqR3Y7FStVtcT8Q1uFaOhPawn4c+mfZX6G+LtVKlUthDR8dyylkZWaGI30mci1zNbq8Eia15E+c+QT5z2w3gTOBeY4tfwVzQK"
    "Y75nizd8v3VOTwnwaSM4twEdQ+Gn5Mwjn2ubTXL/5/H3JoAu4OlbdJCYer3evJbSAQe0Tdz16TiOF9qW0Wj8GB0cDfIY9X+cwB/7"
    "fCF00flS0gAAAABJRU5ErkJggg==",
};
static const char* const kHorseFrames[] = {
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MTg6NTMAwDZRJQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzoyMTozOCswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MTg6NTMrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgpRm1/YAAACQ0lEQVRYCe3US0sWURjA8el+zwvdqDAXkiSVfYAkCaEWUQQtC6Vl"
    "tAgSIVcu2tTGWrjyK7RqURBGCxGEWmSJ1KKi0DLppnaxu///y5mXcVAaBQVpHvhxzpn3PTPPec6ciaI88grkFfjfK7BkHguwlHuX"
    "4GB4RhftBP6G8bw1K7nzThzDDXzBe7RhG+a06OVMLIU3rsIOlGE1XOkybMRuNOI2vsHVxsbpX8ZWFJPwxunwZvuwBauwNvQraJ1s"
    "WT/gE1zZKIztcF491sH4hTf4DX+/gD8wkR+I0gk4Po0mmIA3+I4arEExc/qGN/tZ6EXRClqrkYxBBlcwjFocwgH4nEICtFOigdET"
    "WDazHsCtMI5LmbX9yrxrWA/Ddi9MxCoXwkyScZTBrnDhPu0dbArj2TYPmdCOz2GibX/oT9tUcrUXltVVtsL9PoK4Kl63MukXLF0V"
    "34s9SG8Zl2aO8/w0Ah9wE9VwvyzjS3jdsl6EL2QHXsNrvismYfImdxazjhPMuIceWHaP2Dk8ggk9wyV4KgxX57luhJUzARN1TuaV"
    "F18GJg1jAtcxBFf1Av6nJozd13f4iPgEbKZfhw3owlVYhUyRTMBj0QfPeByWdwxVeIAyHMZbeL73owXOHYD/vQsXkimS59YSKh1+"
    "St1/t8IzbZxCJc7Aj47VczHPEX8X6P47khWY6d8eVff0KV7B1ZXDJB6jE26N1euGX8fpFsLluYVV8sXzS2e4arfjONz3BY9mnngS"
    "Waq34MnlD8wrkFdg8VVgEv4NkYesb1XJAAAAAElFTkSuQmCC",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MTg6NTMAwDZRJQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzoyMTo1MSswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MTg6NTMrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgrKnyCnAAACDUlEQVRYCe3US0hUURzH8SkzYQZCVFIDjRlRMEGkraIoLVrYQgKR"
    "AnEEdyoG4lI36qZAURDcCO5EUHDRpkQmUIighRu12uSAb3xrGj6/P5gjVxydqzi48P7hw5w599zz+J9zj8vlhJMBJwP3IAPxrPHp"
    "ZeuMuezBLdQ/oo88VCIXf7CLc6FG0YhsOtXAr+DDf3jQhRVEJeLoNR/dmME2TizWKPuhdmfx4KwUvvCQajcK8RZeHGIJswhiB2l4"
    "DaVcK42FQhM4gvqRRZRjAse4ENqSCqjBNBZCNKh1NXbKGvgbXqIMY1BWAtCiLoSy8R6rMDPXJEZC/+0Mam3zm/eKYOIJBT9KEfbs"
    "pfBgFJq5OtrCRzRjD9bOI5WV3lrYjse0rIP21nQ+TjkHSuEgNmGeLVM+sPw39eb3L88SYTt8tPwMrVTm8Q46OLorCvADZoAeym2h"
    "On3bpt78NlFnK8xe6KYK4iu08ikEoFRqAulIgs5DCXQ2+jEMZagamdC+60t4ATf+4cowE1inVS+077oo9KLOgglNRJfJM8RCE9Zq"
    "f0Lti/ELnfChEQ1oh61Qp5qMvoRwoefJqMIk9vEdb+BFH2qgPpSxAbQgKqGbrAM6hDqYuvW0bfq8nqMeX6BJRwzN9rqhrdFeezCE"
    "VHzCHFqh7dEkNhD1yGCED8iCMpOAmyyK15xwMuBkwMnAHWXgFOcbnaDbuwxNAAAAAElFTkSuQmCC",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MTg6NTMAwDZRJQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzoyMTo1NiswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MTg6NTMrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgpIUcDXAAACeUlEQVRYCe3Vy0sVURzAcb1Z9qboKZElRmX0hiCK0CJsWS3CbbZp"
    "U7sIBEEJ2ke0KPoPilZRqyKpNqGLUgQLJdKKsscie/jo9f1Ocy5zp6tk5Kr5wYeZOXdmzu/8zjlzS0qyyCqQVeB/r0DpFBdgOu8v"
    "i33j+Dnd31QkMI1O5mMpNmMFVuMFLuAL8mF2/yrm8aJKrMVu1GM9rIIxiH5cxXdEUawCoe1HfE+O4zKswgI4wq8Ywghmw9+3YSc2"
    "YQnSYadtaISJRJGswCxa9mE7ZsCX25EdrIOjK5bAHNotdwVMLhnDXNxANbZiIw7hIkaRD0fdjMf4CB/02I73sBqT5aK7BJPfgctw"
    "ET5AHQpiP1fpjsZoO41XmGzn3n8XTksOhouxCS7EKuTDFdsL58iSW5pQOhdTP/4mgXM8twghrLJTOxOeR+EaOArn9y0sT2D5Tcap"
    "SMYAFy+xBs6/867wUo8mbEUdVLi2rWDeuY4+Es84XsFN3MYb+KBRjnfR2a9t5IJ8hJOwcyu0BVVwEXv/Qvjbifh4luMHjBtm7+pO"
    "7ghvtlSH8RwPcQzXcB3OZzIcpc+b4ErU4xY6cRCWvmjYuaVxzsOow427OGmBI26FL6yGH5c7GEQyfH4MjtZp83vgAt6A5XAtfUJB"
    "mMB44X61pGdgBYzFqIOdd8Otlg6reQS++zy64DQdgM+9hoOOYqIE9nKHu+Me3B0+5IhcrHvgx6cPVi8Zc7lw+u7jKfwPMAmr0Bhf"
    "W40oJkqgljssp/NoAoZ/JE9gIg2oQQdGEMLzASQXs0n2xEwqvbNo+j2O0+SWtJzpyNFQiTacwp+Gi1VZZBXIKpBVIF+BnznutbO1"
    "WJudAAAAAElFTkSuQmCC",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MTg6NTMAwDZRJQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzoyMjowNCswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MTg6NTMrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgr2Oh0QAAACcElEQVRYCe3UW2iOcRzA8YeNd29oDhtmY0s5rCSL3BhiyiGlFJkU"
    "cuXCnYvdrZTdGFdS4kJGa9y4U9o1JkUOE70jlCli5TSHme/36X3Ws2nv3t5I6fnVZ8/h//xPv//vXRAkkWQgyUCSgX+cgXF/cf7x"
    "jF2CNCaiKPvcz/UVfiL4kwsoZrzyrElcp2M+qjEVvpuFZ2hBDwI7jYwUL/z4E75mG/1uHhxoEO5C3rvLGjjZcixAGWajFGYiHst4"
    "eIST6I8vwBTZeTsW4S1M1XdUYTFcgKmLL8AU16ASkzFa9NJgBqZhFa4gg6HYxF0n3sNJ3L33dvTe3RbCTB7FFnTAsV6gESl3PQF7"
    "sB8WSy2+wMW0w3fuvpD4RqdDOINu3IHz1cNMdCFYD1dzEJ9h2g/DgnLySyhk5/bJoApRWPQLcRwHMAVhePbPcRWb4SotxDV4CAcz"
    "dTfRjLNwR31w0ba5W49uANaN952YiTHDVVbAijU1O3ENH+FgLq4NcxFFmpslMHtHcBoW7j2cw21YQxZ1vNh5HB7WweqsDVybcANW"
    "+kt0oAE5B6F9BR5gN0x1Hc7jLrbB4/wtnNyB98KfRjVMpUfwBqdwDN0wpbliH40ptMLFv0YXnNhFeTRP4fhD4UqNGZgT3gXBSq7+"
    "Q7mM6/B8xwrH8XdtnwsjPi7heR2sLY/IjD5GGGbA8Gfn+W7FUrTBghu2Wp5HizIadqEF7j4eP3jIwN3XYiM+oBcD0QIqeTiBJ/Dc"
    "7sPiyzd28GEFLubo8I42x03D4+6B78KwDkrheUXHEjbk+Wct391CYx7fO5eLiDafR5fkkyQDSQb+5wz8ApM5nnzR85qXAAAAAElF"
    "TkSuQmCC",
    "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAMZlWElmTU0AKgAAAAgABgESAAMAAAABAAEA"
    "AAEaAAUAAAABAAAAVgEbAAUAAAABAAAAXgEoAAMAAAABAAIAAAExAAIAAAAVAAAAZodpAAQAAAABAAAAfAAAAAAAAABIAAAAAQAA"
    "AEgAAAABUGl4ZWxtYXRvciBQcm8gMy43LjEAAAAEkAQAAgAAABQAAACyoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEA"
    "AAAgAAAAADIwMjY6MDE6MjggMjM6MTg6NTMAwDZRJQAAAAlwSFlzAAALEwAACxMBAJqcGAAAA7BpVFh0WE1MOmNvbS5hZG9iZS54"
    "bXAAAAAAADx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IlhNUCBDb3JlIDYuMC4wIj4KICAgPHJk"
    "ZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4KICAgICAgPHJkZjpE"
    "ZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEu"
    "MC8iCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIgogICAgICAgICAgICB4bWxu"
    "czp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iPgogICAgICAgICA8dGlmZjpZUmVzb2x1dGlvbj43MjAwMDAvMTAw"
    "MDA8L3RpZmY6WVJlc29sdXRpb24+CiAgICAgICAgIDx0aWZmOlhSZXNvbHV0aW9uPjcyMDAwMC8xMDAwMDwvdGlmZjpYUmVzb2x1"
    "dGlvbj4KICAgICAgICAgPHRpZmY6UmVzb2x1dGlvblVuaXQ+MjwvdGlmZjpSZXNvbHV0aW9uVW5pdD4KICAgICAgICAgPHRpZmY6"
    "T3JpZW50YXRpb24+MTwvdGlmZjpPcmllbnRhdGlvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxZRGltZW5zaW9uPjMyPC9leGlmOlBp"
    "eGVsWURpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6UGl4ZWxYRGltZW5zaW9uPjMyPC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAg"
    "ICAgICAgPHhtcDpNZXRhZGF0YURhdGU+MjAyNi0wMS0yOFQyMzoyMjoxMiswOTowMDwveG1wOk1ldGFkYXRhRGF0ZT4KICAgICAg"
    "ICAgPHhtcDpDcmVhdGVEYXRlPjIwMjYtMDEtMjhUMjM6MTg6NTMrMDk6MDA8L3htcDpDcmVhdGVEYXRlPgogICAgICAgICA8eG1w"
    "OkNyZWF0b3JUb29sPlBpeGVsbWF0b3IgUHJvIDMuNy4xPC94bXA6Q3JlYXRvclRvb2w+CiAgICAgIDwvcmRmOkRlc2NyaXB0aW9u"
    "PgogICA8L3JkZjpSREY+CjwveDp4bXBtZXRhPgqQauEZAAACRUlEQVRYCe3US0tVURTA8dtDytKsG5FhglCQRgZGkZCFk8AGgeNm"
    "gjiK/AA5KhCJKGjQJKpBn6AHpA1q0iAnPQeJhqVFIJHSS83K+v8PZ8Ph4PVeopx4FvzYtu85a++99jrlcllkFcgqsNwrsGIJCrCK"
    "NbQSZfiGeUTxPzbgQutQFdvGuB3lqMdlDCOK1eGPfzCuIUctdqEJB9AIF/fkIfL80Yk5J9IVcNc74PgFX/ETFdgIT2ZM43vMQ/hb"
    "A1pxBC5SKHyvFY98wJctWR0O4hD2wg18js0wmlCW0XBuFp7C+90ET+pG04fymSf4hGZswCm8gP2Q241rGIVJf8fcwH30Yz6eC7+V"
    "OlrBG3BhK9SDKZi7C1EFJhkfwBMO4iF88SVc+CTSp2KqaHjymziNdzDXJXh95nyPKExuWWpQCRvGq3G+GldQ6omTz33gvWNIb978"
    "ruMaBcOOPoE7GMEv/MAYTLoP5+E92qw2VrimMD5mzuYsOcJObagzMPFb2ED2wVUcRnLna/m3zXsd3usbDMFKRHfMuGiEZFt5ag9m"
    "0Yb9eAoXvQWT+zmmw+fHYQVGcRZufACvUDTCBjbz5FHYBx/RjT54jzZpobB32lGPC7iLcvh5eW1FI2zAjr+InbCEnfB0zifDBmqG"
    "yavRhC3wU7sNO9z3rMp6FI2wAe9sImYveIJzeI1kmLQFeVj2ZxiCz/npGjbhPdhLfx1+JqEpk0ncsCevjUcrslAcZ7J3oR+Waq6R"
    "hZ6jA/5Xn0VWgawCWQUKVuAPuZOWYXRV+iIAAAAASUVORK5CYII=",
};

static std::vector<BYTE> Base64Decode(const char* s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<BYTE> out;
    int buf = 0, bits = 0;
    for (; *s; ++s) {
        int v = val(*s);
        if (v < 0) continue;                    // skips '=' padding
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(BYTE((buf >> bits) & 0xFF)); }
    }
    return out;
}

struct Character {
    std::vector<G::Bitmap*> frames;
    std::vector<float> delays;                  // ms per frame (GIF only)
    bool tint = false;                          // silhouette frames get recolored to the theme
    float aspect = 1;
    int frame = 0;
    float acc = 0;
    DWORD loadedMode = ~0u;
    std::wstring loadedGif;
    void Clear() {
        for (G::Bitmap* f : frames) delete f;
        frames.clear();
        delays.clear();
        frame = 0;
        acc = 0;
    }
};
static Character g_char;

// Copies a frame into a standalone premultiplied bitmap (scaled to at most maxH pixels high), so
// the source image/stream/file can be released immediately.
static G::Bitmap* CopyFrame(G::Image* src, UINT maxH) {
    UINT w = src->GetWidth(), h = src->GetHeight();
    if (!w || !h) return nullptr;
    UINT ch = std::min(h, maxH), cw = std::max<UINT>(1, UINT(double(w) * ch / h));
    auto* copy = new G::Bitmap(cw, ch, PixelFormat32bppPARGB);
    G::Graphics cg(copy);
    cg.SetInterpolationMode(G::InterpolationModeHighQualityBicubic);
    cg.Clear(G::Color(0, 0, 0, 0));
    cg.DrawImage(src, 0, 0, cw, ch);
    return copy;
}

static void EnsureCharacter() {
    if (g_char.loadedMode == g_set.character && (g_set.character != kCharGif || g_char.loadedGif == g_set.characterGif))
        return;
    g_char.Clear();
    g_char.loadedMode = g_set.character;
    g_char.loadedGif = g_set.characterGif;
    std::vector<const char*> builtin;
    if (g_set.character == kCharCat) builtin.assign(std::begin(kCatFrames), std::end(kCatFrames));
    if (g_set.character == kCharParrot) builtin.assign(std::begin(kParrotFrames), std::end(kParrotFrames));
    if (g_set.character == kCharHorse) builtin.assign(std::begin(kHorseFrames), std::end(kHorseFrames));
    if (!builtin.empty()) {
        for (const char* b64 : builtin) {
            std::vector<BYTE> png = Base64Decode(b64);
            IStream* st = SHCreateMemStream(png.data(), UINT(png.size()));
            if (!st) continue;
            G::Bitmap src(st);
            if (src.GetLastStatus() == G::Ok)
                if (G::Bitmap* f = CopyFrame(&src, 64)) g_char.frames.push_back(f);
            st->Release();
        }
        g_char.tint = true;
        g_char.aspect = 1;
    } else if (g_set.character == kCharGif && !g_set.characterGif.empty()) {
        G::Bitmap src(g_set.characterGif.c_str());
        if (src.GetLastStatus() != G::Ok) return;
        UINT dims = src.GetFrameDimensionsCount();
        std::vector<GUID> ids(std::max<UINT>(dims, 1));
        UINT count = 1;
        if (dims) { src.GetFrameDimensionsList(ids.data(), dims); count = std::max<UINT>(1, src.GetFrameCount(&ids[0])); }
        count = std::min<UINT>(count, 200);
        std::vector<long> delays;
        if (UINT size = src.GetPropertyItemSize(PropertyTagFrameDelay)) {
            std::vector<BYTE> buf(size);
            auto* item = (G::PropertyItem*)buf.data();
            if (src.GetPropertyItem(PropertyTagFrameDelay, size, item) == G::Ok)
                delays.assign((long*)item->value, (long*)item->value + item->length / sizeof(long));
        }
        for (UINT i = 0; i < count; ++i) {
            if (dims) src.SelectActiveFrame(&ids[0], i);
            if (G::Bitmap* f = CopyFrame(&src, 96)) {
                g_char.frames.push_back(f);
                long d = i < delays.size() ? delays[i] * 10 : 100;    // GIF delays are 1/100 s
                g_char.delays.push_back(float(std::max(20L, d)));
            }
        }
        g_char.tint = false;
        if (!g_char.frames.empty())
            g_char.aspect = float(g_char.frames[0]->GetWidth()) / g_char.frames[0]->GetHeight();
    }
}

// On-screen size of the character in pixels. RunCat silhouettes are drawn at a fixed height.
// Small pixel-art GIFs are scaled by a whole number with nearest-neighbor sampling so every pixel
// stays crisp; larger GIFs are smoothly scaled down to the target height.
struct CharSize { float w = 0, h = 0; bool nearest = false; };
static CharSize CharacterSize(float S) {
    CharSize cs;
    if (g_char.frames.empty()) return cs;
    float h0 = float(g_char.frames[0]->GetHeight());
    if (g_char.tint) {
        cs.h = 30 * S;
    } else if (h0 <= 48) {
        float target = 38 * S, maxH = 46 * S;
        int k = std::max(1, int(std::lround(target / h0)));
        while (k > 1 && h0 * k > maxH) --k;
        cs.h = h0 * k;
        cs.nearest = true;
    } else {
        cs.h = std::min(38 * S, h0);
    }
    cs.w = cs.h * g_char.aspect;
    float maxW = 80 * S;                        // very wide GIFs shrink rather than eat the taskbar
    if (cs.w > maxW) { cs.h *= maxW / cs.w; cs.w = maxW; cs.nearest = false; }
    return cs;
}

// Logical (96-dpi) width of the character slot at the left of the widget; 0 when hidden.
static float CharSlotW(float S) {
    EnsureCharacter();
    if (g_set.character == kCharNone || g_char.frames.empty()) return 0;
    return CharacterSize(S).w / S + 7;
}

// How busy the chosen source is, 0..100. Network is log-scaled: 0 at idle, 100 at 100 Mbps.
static float g_runCpu = 0, g_runRam = 0, g_runNetBps = 0;
static float RunnerLoad() {
    switch (g_set.speedSource) {
    case kSrcRam: return g_runRam;
    case kSrcNet: {
        float kbps = g_runNetBps * 8 / 1000;
        return std::min(100.0f, std::log10(1 + kbps) / 5 * 100);
    }
    default: return g_runCpu;
    }
}

// RunCat's pacing: frame time = 500 ms / max(1, load/5 * rate); rate 0.5 caps at our 20 fps.
// GIFs keep their own timing, played from 0.5x (idle) up to ~3.5x (load at 100%).
static void AdvanceCharacter(float dtMs, float load) {
    int n = int(g_char.frames.size());
    if (n < 2) return;
    g_char.acc += dtMs;
    for (int guard = 0; guard < 8; ++guard) {
        float interval = g_char.tint ? 500.0f / std::max(1.0f, load / 5.0f * 0.5f)
                                     : g_char.delays[g_char.frame] / (0.5f + load / 33.0f);
        if (g_char.acc < interval) break;
        g_char.acc -= interval;
        g_char.frame = (g_char.frame + 1) % n;
    }
    g_char.acc = std::min(g_char.acc, 1000.0f);
}

// Card/ring geometry in 96-dpi units.
struct Geo { float pad = 3, gap = 5, vpad = 5, netW = 104, metW = 62; };
static Geo GeoFor(DWORD style) {
    Geo g;
    if (style == kStyleRings) g.metW = 62;
    return g;
}
static int MetricCount() { return g_set.extras ? 4 : 2; }

// Subtle: two rows, columns of [icon value] pairs, faint per-value sparklines (96-dpi units).
struct SubtleGeo { float pad = 4, gap = 9, netW = 70, metW = 40; };
static int SubtleColumns() { return g_set.extras ? 2 : 1; }

static int StyleWidth(const Layout& L, int dpi);

static int WidgetWidth(const Layout& L, int dpi) {
    float S = dpi / 96.0f;
    return StyleWidth(L, dpi) + int(std::ceil(CharSlotW(S) * S));
}

static int StyleWidth(const Layout& L, int dpi) {
    if (g_set.style == kStyleSubtle) {
        SubtleGeo sg;
        float w = sg.pad * 2 + sg.netW + SubtleColumns() * (sg.gap + sg.metW);
        return int(std::ceil(w * dpi / 96.0f));
    }
    if (g_set.style == kStyleClassic) {
        int w = L.pad + L.colNet + L.gap + L.colPct + L.pad;
        if (g_set.extras) w += L.gap + L.colPct;
        return w;
    }
    Geo g = GeoFor(g_set.style);
    float w = g.pad * 2 + g.netW + MetricCount() * (g.gap + g.metW);
    return int(std::ceil(w * dpi / 96.0f));
}

static void RestoreTaskList() {
    if (!g_dock.win11 && g_dock.shrunk && IsWindow(g_dock.taskList)) {
        RECT pc;
        GetClientRect(g_dock.parent, &pc);
        const RECT& r = g_dock.taskListOrig;
        bool vertical = (pc.bottom - pc.top) > (pc.right - pc.left);
        if (!vertical) MoveWindow(g_dock.taskList, r.left, r.top, pc.right - r.left, r.bottom - r.top, TRUE);
        else            MoveWindow(g_dock.taskList, r.left, r.top, r.right - r.left, pc.bottom - r.top, TRUE);
    }
    g_dock.shrunk = false;
}

// Size and place the widget inside the taskbar, re-shrinking the Win10 task list if Explorer
// grew it back. Returns false if the taskbar layout is gone (Explorer restarting).
static bool Reposition() {
    if (!g_widget || !IsWindow(g_dock.parent)) return false;
    RECT pc;
    GetClientRect(g_dock.parent, &pc);
    int pw = pc.right - pc.left, ph = pc.bottom - pc.top;
    bool vertical = ph > pw;
    int dpi = DpiOf(g_dock.tray);
    Layout L = Measure(dpi);
    int margin = MulDiv(4, dpi, 96);

    int w = vertical ? pw : WidgetWidth(L, dpi);
    int rows = vertical ? (g_set.extras ? 6 : 4) : 2;
    int h = vertical ? L.rowH * rows + 2 * L.pad : ph;
    int x = 0, y = 0;

    if (!g_dock.win11) {
        RECT tl;
        GetWindowRect(g_dock.taskList, &tl);
        MapWindowPoints(nullptr, g_dock.parent, (POINT*)&tl, 2);
        if (!g_dock.shrunk) { g_dock.taskListOrig = tl; g_dock.shrunk = true; }
        const RECT& o = g_dock.taskListOrig;
        if (!vertical) {
            // Left: widget takes the start of the band and the task list follows it.
            // Right: task list stops short of the band's end and the widget fills the rest.
            int l = g_set.posRight ? o.left : o.left + w, r = g_set.posRight ? pw - w : pw;
            if (tl.left != l || tl.right != r) MoveWindow(g_dock.taskList, l, o.top, r - l, o.bottom - o.top, TRUE);
            x = g_set.posRight ? pw - w : o.left;
            y = 0;
        } else {
            int t = g_set.posRight ? o.top : o.top + h, b = g_set.posRight ? ph - h : ph;
            if (tl.top != t || tl.bottom != b) MoveWindow(g_dock.taskList, o.left, t, o.right - o.left, b - t, TRUE);
            x = 0;
            y = g_set.posRight ? ph - h : o.top;
        }
    } else {
        HWND notify = FindWindowExW(g_dock.tray, nullptr, L"TrayNotifyWnd", nullptr);
        int rightEdge = pw - MulDiv(200, dpi, 96);
        if (notify) {
            RECT nr;
            GetWindowRect(notify, &nr);
            MapWindowPoints(nullptr, g_dock.parent, (POINT*)&nr, 2);
            rightEdge = nr.left;
        }
        std::vector<std::pair<int, int>> spans;
        EnterCriticalSection(&g_btnLock);
        for (RECT r : g_buttons) {
            MapWindowPoints(nullptr, g_dock.parent, (POINT*)&r, 2);
            if (r.left < rightEdge && r.right - r.left < pw / 2) spans.push_back({ r.left, r.right });
        }
        LeaveCriticalSection(&g_btnLock);
        x = FindFreeX(spans, rightEdge, w, margin, !g_set.posRight);
        if (x < 0) x = g_set.posRight ? rightEdge - w - margin : margin;   // taskbar is full
    }

    // HWND_TOP keeps us above the Win11 XAML bridge, which covers the whole taskbar.
    SetWindowPos(g_widget, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

static bool g_tipShown = false, g_mouseTracked = false;
static const UINT_PTR kHoverTimerId = 2;

static void ShowTooltip(bool show);

static void Detach() {
    ShowTooltip(false);
    RestoreTaskList();
    if (g_widget && IsWindow(g_widget)) DestroyWindow(g_widget);
    g_widget = nullptr;
}

// ---- Details popup -------------------------------------------------------------------------
// A self-drawn popup instead of a comctl32 tooltip: without a v6 manifest (which a single .cpp
// built by g++ cannot embed) Windows hands out comctl32 5.82, whose tracking tooltips crashed
// here. Lifetime is the app's; it is owned by the hidden controller, not by Explorer's taskbar.
static const wchar_t kTipClass[] = L"TaskMonTip";
static wchar_t g_tipText[1024] = L"";
static HFONT g_tipFont = nullptr;
static int g_tipFontDpi = 0;

static bool CursorOverWidget() {
    POINT pt;
    return g_widget && GetCursorPos(&pt) && WindowFromPoint(pt) == g_widget;
}

static void EnsureTipFont(int dpi) {
    if (g_tipFont && g_tipFontDpi == dpi) return;
    if (g_tipFont) DeleteObject(g_tipFont);
    g_tipFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_tipFontDpi = dpi;
}

static LRESULT CALLBACK TipProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(g_light ? RGB(249, 249, 249) : RGB(43, 43, 43));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HBRUSH border = CreateSolidBrush(g_light ? RGB(200, 200, 200) : RGB(80, 80, 80));
        FrameRect(dc, &rc, border);
        DeleteObject(border);
        int pad = MulDiv(10, g_tipFontDpi ? g_tipFontDpi : 96, 96);
        InflateRect(&rc, -pad, -pad);
        HGDIOBJ old = SelectObject(dc, g_tipFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_light ? RGB(25, 25, 25) : RGB(235, 235, 235));
        DrawTextW(dc, g_tipText, -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Size the popup to its text and place it just above the widget (below for a top taskbar).
static void PlaceTooltip() {
    if (!g_tip || !g_widget) return;
    int dpi = DpiOf(g_widget);
    EnsureTipFont(dpi);
    HDC dc = GetDC(g_tip);
    HGDIOBJ old = SelectObject(dc, g_tipFont);
    RECT tr{ 0, 0, 0, 0 };
    DrawTextW(dc, g_tipText, -1, &tr, DT_CALCRECT | DT_LEFT | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g_tip, dc);
    int pad = MulDiv(10, dpi, 96);
    int tw = tr.right + 2 * pad, th = tr.bottom + 2 * pad;

    RECT wr;
    GetWindowRect(g_widget, &wr);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST), &mi);
    int gap = MulDiv(6, dpi, 96);
    int x = std::min(std::max<int>(wr.left, mi.rcMonitor.left), int(mi.rcMonitor.right) - tw);
    int y = wr.top - th - gap;
    if (y < mi.rcMonitor.top) y = wr.bottom + gap;
    SetWindowPos(g_tip, HWND_TOPMOST, x, y, tw, th, SWP_NOACTIVATE);
    InvalidateRect(g_tip, nullptr, TRUE);
}

static void ShowTooltip(bool show) {
    if (!g_tip) return;
    if (show && g_widget) {
        PlaceTooltip();
        ShowWindow(g_tip, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(g_tip, SW_HIDE);
    }
    g_tipShown = show && g_widget;
}

static void CreateTipWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = TipProc;
    wc.lpszClassName = kTipClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTipClass, L"", WS_POPUP,
                            0, 0, 10, 10, g_ctrl, nullptr, inst, nullptr);
    // Rounded corners on Windows 11 (DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUNDSMALL = 3).
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT(WINAPI * Fn)(HWND, DWORD, LPCVOID, DWORD);
        if (Fn fn = (Fn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            DWORD pref = 3;
            fn(g_tip, 33, &pref, sizeof(pref));
        }
    }
}

static bool Attach() {
    Detach();
    if (!FindDock(g_dock)) return false;
    g_light = ReadLightTheme();
    // Same mechanism as TrafficMonitor: a child window whose parent lives in explorer.exe.
    g_widget = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kWidgetClass, L"TaskMon",
                               WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, g_dock.parent, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!g_widget) return false;
    bool ok = Reposition();
    Render();
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Formatting and painting
// ---------------------------------------------------------------------------------------------
static void FormatRate(wchar_t* buf, size_t n, double bytesPerSec) {
    if (g_set.bits) {
        static const wchar_t* units[] = { L"bps", L"Kbps", L"Mbps", L"Gbps" };
        double v = bytesPerSec * 8;
        int u = 0;
        while (v >= 1000.0 && u < 3) { v /= 1000.0; ++u; }
        swprintf(buf, n, u == 0 || v >= 100 ? L"%.0f %ls" : L"%.1f %ls", v, units[u]);
    } else {
        static const wchar_t* units[] = { L"B/s", L"KB/s", L"MB/s", L"GB/s" };
        double v = bytesPerSec;
        int u = 0;
        while (v >= 1000.0 && u < 3) { v /= 1024.0; ++u; }
        swprintf(buf, n, u == 0 || v >= 100 ? L"%.0f %ls" : L"%.1f %ls", v, units[u]);
    }
}

static void FormatBytes(wchar_t* buf, size_t n, ULONG64 b) {
    static const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = double(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    swprintf(buf, n, u == 0 ? L"%.0f %ls" : L"%.2f %ls", v, units[u]);
}

// ---- History and animation state ------------------------------------------------------------
static const int kHist = 180;
struct History {
    float v[kHist] = {};
    int n = 0;
    void Push(float x) {
        std::copy(v + 1, v + kHist, v);
        v[kHist - 1] = x;
        if (n < kHist) ++n;
    }
    float At(int i) const { return v[kHist - 1 - i]; }           // 0 = newest
    float MaxOf(int count) const {
        float m = 0;
        for (int i = 0; i < std::min(count, n); ++i) m = std::max(m, At(i));
        return m;
    }
};
static History g_hUp, g_hDown, g_hCpu, g_hRam, g_hGpu, g_hDisk;

// A value that glides toward its target instead of jumping once per second.
struct Eased {
    float shown = 0, target = 0;
    bool init = false;
    void Set(float t) { target = t; if (!init) { shown = t; init = true; } }
    void Step(float k) { shown += (target - shown) * k; }
    void Snap() { shown = target; }
};
static Eased g_eUp, g_eDown, g_eCpu, g_eRam, g_eGpu, g_eDisk, g_eNetScale;
static Eased* const g_allEased[] = { &g_eUp, &g_eDown, &g_eCpu, &g_eRam, &g_eGpu, &g_eDisk, &g_eNetScale };
static ULONGLONG g_lastSample = 0, g_lastFrame = 0;

static void PushSamples() {
    const Stats& s = g_stats;
    float gpu = float(std::max(0, s.gpu)), disk = float(std::max(0, s.disk));
    g_hUp.Push(float(s.upBps));   g_eUp.Set(float(s.upBps));
    g_hDown.Push(float(s.downBps)); g_eDown.Set(float(s.downBps));
    g_hCpu.Push(float(s.cpu));    g_eCpu.Set(float(s.cpu));
    g_hRam.Push(float(s.ram));    g_eRam.Set(float(s.ram));
    g_hGpu.Push(gpu);             g_eGpu.Set(gpu);
    g_hDisk.Push(disk);           g_eDisk.Set(disk);
    // Network graphs auto-scale to the recent peak, with a 100 Kbps floor so idle noise stays flat.
    float peak = std::max(g_hUp.MaxOf(60), g_hDown.MaxOf(60));
    g_eNetScale.Set(std::max(peak * 1.25f, 12500.0f));
    g_lastSample = GetTickCount64();
    if (!g_set.animate)
        for (Eased* e : g_allEased) e->Snap();
}

static void StepAnimation() {
    ULONGLONG now = GetTickCount64();
    float dt = float(std::min<ULONGLONG>(now - g_lastFrame, 250));
    g_lastFrame = now;
    float k = 1.0f - std::exp(-dt / 140.0f);
    if (g_set.animate)
        for (Eased* e : g_allEased) e->Step(k);
    g_runCpu = g_eCpu.target;
    g_runRam = g_eRam.target;
    g_runNetBps = g_eUp.target + g_eDown.target;
    AdvanceCharacter(dt, RunnerLoad());
}

// 0..1 progress through the current second, used to scroll graphs continuously.
static float ScrollPhase() {
    if (!g_set.animate) return 0;
    return std::min(1.0f, float(GetTickCount64() - g_lastSample) / 1000.0f);
}

// ---- GDI+ resources --------------------------------------------------------------------------
static G::Color Col(COLORREF c, BYTE a = 255) { return G::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

static const wchar_t* IconFamily() {
    static std::wstring fam;
    if (fam.empty()) {
        G::FontFamily f(L"Segoe Fluent Icons");
        fam = f.IsAvailable() ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";   // Win11 : Win10
    }
    return fam.c_str();
}
static wchar_t IconFor(Item i) {
    bool fluent = wcscmp(IconFamily(), L"Segoe Fluent Icons") == 0;
    switch (i) {
    case kUp:   return 0xE898;                  // Upload
    case kDown: return 0xE896;                  // Download
    case kCpu:  return 0xE950;                  // Processor chip
    case kRam:  return fluent ? 0xEEA0 : 0xE950; // Memory module
    case kGpu:  return fluent ? 0xF211 : 0xE7F4; // Graphics card (monitor on Win10)
    case kDisk: return 0xEDA2;                  // Hard drive
    }
    return L'?';
}

struct FontSet {
    int dpi = 0;
    G::Font *net = nullptr, *value = nullptr, *icon = nullptr, *iconSmall = nullptr, *classic = nullptr;
    G::Font *subtle = nullptr, *subtleIcon = nullptr;
    void Free() {
        delete net; delete value; delete icon; delete iconSmall; delete classic; delete subtle; delete subtleIcon;
        *this = FontSet{};
    }
};
static FontSet g_fs;

static void EnsureFonts(int dpi) {
    if (g_fs.dpi == dpi) return;
    g_fs.Free();
    float S = dpi / 96.0f;
    int classicPx = std::min(MulDiv(9, dpi, 72), MulDiv(17, dpi, 96) - MulDiv(3, dpi, 96));
    g_fs.net = new G::Font(L"Segoe UI Semibold", 11.5f * S, G::FontStyleRegular, G::UnitPixel);
    g_fs.value = new G::Font(L"Segoe UI", 13.0f * S, G::FontStyleBold, G::UnitPixel);
    g_fs.icon = new G::Font(IconFamily(), 13.0f * S, G::FontStyleRegular, G::UnitPixel);
    g_fs.iconSmall = new G::Font(IconFamily(), 10.5f * S, G::FontStyleRegular, G::UnitPixel);
    g_fs.classic = new G::Font(L"Segoe UI Semibold", float(classicPx), G::FontStyleRegular, G::UnitPixel);
    g_fs.subtle = new G::Font(L"Segoe UI", 11.0f * S, G::FontStyleRegular, G::UnitPixel);
    g_fs.subtleIcon = new G::Font(IconFamily(), 9.5f * S, G::FontStyleRegular, G::UnitPixel);
    g_fs.dpi = dpi;
}

// ---- Drawing helpers ------------------------------------------------------------------------
static void AddRoundRect(G::GraphicsPath& p, const G::RectF& r, float rad) {
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

static void Text(G::Graphics& g, const wchar_t* s, const G::Font* f, const G::RectF& r, COLORREF col,
                 G::StringAlignment align, bool shadow) {
    G::StringFormat fmt(G::StringFormatFlagsNoWrap | G::StringFormatFlagsNoClip);
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(G::StringAlignmentCenter);
    fmt.SetTrimming(G::StringTrimmingNone);     // never drop trailing characters like "%"
    if (shadow && !g_light) {                   // keeps text legible over graphs and images
        G::SolidBrush sb(G::Color(140, 0, 0, 0));
        G::RectF r2 = r;
        r2.Offset(0, 1);
        g.DrawString(s, -1, f, r2, &fmt, &sb);
    }
    G::SolidBrush b(Col(col));
    g.DrawString(s, -1, f, r, &fmt, &b);
}

static float Pulse() {                          // 0..1, ~0.8 Hz
    return 0.5f + 0.5f * std::sin(float(GetTickCount64()) / 1000.0f * 5.0f);
}

// Area/line graph. Sample i (0 = newest) sits at right - (i + 1 + phase) * step; an extra "head"
// point at the right edge carries the eased live value, so the curve scrolls and grows smoothly.
static void DrawGraph(G::Graphics& g, const G::RectF& r, const History& h, float headVal, float scale,
                      COLORREF col, float step, float phase, bool fill, float lineW, BYTE lineAlpha,
                      BYTE fillAlpha = 110) {
    if (scale <= 0 || h.n == 0) return;
    auto Y = [&](float v) {
        float f = std::min(std::max(v / scale, 0.0f), 1.0f);
        return r.GetBottom() - f * r.Height;
    };
    std::vector<G::PointF> pts;
    pts.push_back(G::PointF(r.GetRight(), Y(headVal)));
    for (int i = 0; i < h.n; ++i) {
        float x = r.GetRight() - (i + 1 + phase) * step;
        pts.push_back(G::PointF(x, Y(h.At(i))));
        if (x < r.X) break;
    }
    if (pts.size() < 2) return;
    std::reverse(pts.begin(), pts.end());
    if (fill) {
        G::GraphicsPath area;
        area.AddCurve(pts.data(), INT(pts.size()), 0.3f);
        area.AddLine(pts.back().X, r.GetBottom() + 1, pts.front().X, r.GetBottom() + 1);
        area.CloseFigure();
        G::LinearGradientBrush br(G::PointF(0, r.Y - 1), G::PointF(0, r.GetBottom() + 1), Col(col, fillAlpha), Col(col, 0));
        g.FillPath(&br, &area);
    }
    G::GraphicsPath line;
    line.AddCurve(pts.data(), INT(pts.size()), 0.3f);
    G::Pen pen(Col(col, lineAlpha), lineW);
    pen.SetLineJoin(G::LineJoinRound);
    g.DrawPath(&pen, &line);
}

// ---- Background image (cover-cropped, faded, cached per size/opacity) ------------------------
static G::Bitmap* g_bgSrc = nullptr;
static std::wstring g_bgLoaded;
static G::Bitmap* g_bgCache = nullptr;
static int g_bgW = 0, g_bgH = 0;
static DWORD g_bgOp = 0;

static void InvalidateBackground() {
    delete g_bgCache;
    g_bgCache = nullptr;
}

static void EnsureBackground(int w, int h, float S) {
    if (g_set.bgImage.empty()) {
        if (g_bgSrc) { delete g_bgSrc; g_bgSrc = nullptr; }
        g_bgLoaded.clear();
        InvalidateBackground();
        return;
    }
    if (g_bgLoaded != g_set.bgImage) {
        delete g_bgSrc;
        g_bgSrc = G::Bitmap::FromFile(g_set.bgImage.c_str());
        if (g_bgSrc && g_bgSrc->GetLastStatus() != G::Ok) { delete g_bgSrc; g_bgSrc = nullptr; }
        g_bgLoaded = g_set.bgImage;
        InvalidateBackground();
    }
    if (!g_bgSrc) return;
    if (g_bgCache && g_bgW == w && g_bgH == h && g_bgOp == g_set.bgOpacity) return;

    InvalidateBackground();
    g_bgCache = new G::Bitmap(w, h, PixelFormat32bppPARGB);
    G::Graphics cg(g_bgCache);
    cg.SetSmoothingMode(G::SmoothingModeAntiAlias);
    cg.SetInterpolationMode(G::InterpolationModeHighQualityBicubic);
    cg.Clear(G::Color(0, 0, 0, 0));
    G::RectF area(1.5f * S, 3 * S, w - 3 * S, h - 6 * S);
    G::GraphicsPath clip;
    AddRoundRect(clip, area, 8 * S);
    cg.SetClip(&clip);
    // Cover-fit the strip; bias the crop upward because faces/subjects usually sit high in art.
    float sw = float(g_bgSrc->GetWidth()), sh = float(g_bgSrc->GetHeight());
    float scale = std::max(area.Width / sw, area.Height / sh);
    float cw = area.Width / scale, ch = area.Height / scale;
    float sx = (sw - cw) / 2, sy = (sh - ch) * 0.3f;
    G::ColorMatrix cm = { { { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 },
                            { 0, 0, 0, g_set.bgOpacity / 100.0f, 0 }, { 0, 0, 0, 0, 1 } } };
    G::ImageAttributes attr;
    attr.SetColorMatrix(&cm);
    cg.DrawImage(g_bgSrc, area, sx, sy, cw, ch, G::UnitPixel, &attr);
    g_bgW = w; g_bgH = h; g_bgOp = g_set.bgOpacity;
}

// ---- Styles -----------------------------------------------------------------------------------
static void CardBackground(G::Graphics& g, const G::RectF& r, float rad, float S, bool alert) {
    G::GraphicsPath p;
    AddRoundRect(p, r, rad);
    bool img = g_bgCache != nullptr;
    G::SolidBrush fill(g_light ? G::Color(img ? 120 : 22, img ? 255 : 0, img ? 255 : 0, img ? 255 : 0)
                               : G::Color(img ? 95 : 26, img ? 0 : 255, img ? 0 : 255, img ? 0 : 255));
    g.FillPath(&fill, &p);
    if (alert) {
        BYTE a = BYTE(90 + 140 * Pulse());
        G::Pen glow(G::Color(a, 248, 113, 113), 1.6f * S);
        g.DrawPath(&glow, &p);
    } else {
        G::Pen border(g_light ? G::Color(30, 0, 0, 0) : G::Color(34, 255, 255, 255), 1.0f * S);
        g.DrawPath(&border, &p);
    }
}

static void PctText(wchar_t* buf, float v) { swprintf(buf, 16, L"%d%%", int(std::lround(v))); }

static void DrawNetworkCard(G::Graphics& g, const G::RectF& r, float S, bool background) {
    float rad = 7 * S, step = 3 * S, phase = ScrollPhase();
    if (background) CardBackground(g, r, rad, S, false);
    G::GraphicsPath clip;
    AddRoundRect(clip, r, rad);
    g.SetClip(&clip);
    G::RectF gr(r.X, r.Y + 3 * S, r.Width, r.Height - 3 * S);
    DrawGraph(g, gr, g_hDown, g_eDown.shown, g_eNetScale.shown, ItemColor(kDown), step, phase, true, 1.4f * S, 230);
    DrawGraph(g, gr, g_hUp, g_eUp.shown, g_eNetScale.shown, ItemColor(kUp), step, phase, false, 1.4f * S, 235);
    g.ResetClip();

    wchar_t up[32], down[32], ic[2] = { 0, 0 };
    FormatRate(up, 32, g_eUp.shown);
    FormatRate(down, 32, g_eDown.shown);
    float rowH = r.Height / 2, inset = 7 * S;
    struct Row { Item item; const wchar_t* text; } rows[] = { { kUp, up }, { kDown, down } };
    for (int i = 0; i < 2; ++i) {
        G::RectF rr(r.X + inset, r.Y + i * rowH, r.Width - 2 * inset, rowH);
        ic[0] = IconFor(rows[i].item);
        Text(g, ic, g_fs.iconSmall, rr, ItemColor(rows[i].item), G::StringAlignmentNear, true);
        Text(g, rows[i].text, g_fs.net, rr, TextColor(), G::StringAlignmentFar, true);
    }
}

struct MetricRef { Item item; const History* h; const Eased* e; bool available; };

static void DrawMetricCard(G::Graphics& g, const G::RectF& r, float S, const MetricRef& m) {
    float rad = 7 * S;
    bool alert = m.available && m.e->target >= 90;
    CardBackground(g, r, rad, S, alert);
    G::GraphicsPath clip;
    AddRoundRect(clip, r, rad);
    g.SetClip(&clip);
    G::RectF gr(r.X, r.Y + 4 * S, r.Width, r.Height - 4 * S);
    if (m.available) DrawGraph(g, gr, *m.h, m.e->shown, 100, ItemColor(m.item), 3 * S, ScrollPhase(), true, 1.3f * S, 210);
    g.ResetClip();

    wchar_t ic[2] = { IconFor(m.item), 0 }, val[16];
    if (m.available) PctText(val, m.e->shown); else wcscpy(val, L"--");
    G::RectF tr(r.X + 6 * S, r.Y, r.Width - 12 * S, r.Height);
    Text(g, ic, g_fs.icon, tr, ItemColor(m.item), G::StringAlignmentNear, true);
    Text(g, val, g_fs.value, tr, alert ? LoadColor(100) : TextColor(), G::StringAlignmentFar, true);
}

static void DrawMetricRing(G::Graphics& g, const G::RectF& r, float S, const MetricRef& m) {
    float d = std::min(r.Height - 4 * S, 29 * S), th = 3.0f * S;
    G::RectF ring(r.X + 2 * S + th / 2, r.Y + (r.Height - d) / 2 + th / 2, d - th, d - th);
    G::Pen track(Col(TrackColor(), g_light ? 200 : 160), th);
    g.DrawEllipse(&track, ring);
    float pct = m.available ? std::min(std::max(m.e->shown, 0.0f), 100.0f) : 0;
    bool alert = m.available && m.e->target >= 90;
    if (pct > 0.5f) {
        if (alert) {                            // soft pulsing halo behind the arc
            G::Pen halo(Col(ItemColor(m.item), BYTE(40 + 70 * Pulse())), th * 2.4f);
            halo.SetStartCap(G::LineCapRound);
            halo.SetEndCap(G::LineCapRound);
            g.DrawArc(&halo, ring, -90, 3.6f * pct);
        }
        G::Pen arc(Col(ItemColor(m.item)), th);
        arc.SetStartCap(G::LineCapRound);
        arc.SetEndCap(G::LineCapRound);
        g.DrawArc(&arc, ring, -90, 3.6f * pct);
    }
    wchar_t ic[2] = { IconFor(m.item), 0 }, val[16];
    G::RectF iconBox(r.X + 2 * S, r.Y + (r.Height - d) / 2, d, d);
    Text(g, ic, g_fs.iconSmall, iconBox, ItemColor(m.item), G::StringAlignmentCenter, false);
    if (m.available) PctText(val, m.e->shown); else wcscpy(val, L"--");
    G::RectF tr(iconBox.GetRight() + 3 * S, r.Y, r.GetRight() - iconBox.GetRight() - 3 * S, r.Height);
    Text(g, val, g_fs.net, tr, alert ? LoadColor(100) : TextColor(), G::StringAlignmentNear, true);
}

static void DrawModern(G::Graphics& g, int h, float S, bool rings) {
    Geo geo = GeoFor(g_set.style);
    float x = geo.pad * S, y = geo.vpad * S, ch = h - 2 * geo.vpad * S;
    DrawNetworkCard(g, G::RectF(x, y, geo.netW * S, ch), S, true);
    x += (geo.netW + geo.gap) * S;
    MetricRef metrics[] = {
        { kCpu, &g_hCpu, &g_eCpu, true },
        { kRam, &g_hRam, &g_eRam, true },
        { kGpu, &g_hGpu, &g_eGpu, g_stats.gpu >= 0 },
        { kDisk, &g_hDisk, &g_eDisk, g_stats.disk >= 0 },
    };
    for (int i = 0; i < MetricCount(); ++i) {
        G::RectF r(x, y, geo.metW * S, ch);
        if (rings) DrawMetricRing(g, r, S, metrics[i]);
        else DrawMetricCard(g, r, S, metrics[i]);
        x += (geo.metW + geo.gap) * S;
    }
}

// Subtle: no chrome at all. Each value gets a tiny tinted icon and a faint sparkline tinted in
// the same color, drawn underneath the text so the widget reads as quiet text that "breathes".
static void DrawSubtle(G::Graphics& g, int h, float S) {
    SubtleGeo sg;
    float rowH = 16 * S, top = (h - 2 * rowH) / 2, phase = ScrollPhase(), step = 2 * S;
    wchar_t up[32], down[32];
    FormatRate(up, 32, g_eUp.shown);
    FormatRate(down, 32, g_eDown.shown);

    struct Cell { Item item; const History* hist; const Eased* e; float scale; bool pct; bool available; };
    Cell cols[3][2] = {
        { { kUp, &g_hUp, &g_eUp, g_eNetScale.shown, false, true },
          { kDown, &g_hDown, &g_eDown, g_eNetScale.shown, false, true } },
        { { kCpu, &g_hCpu, &g_eCpu, 100, true, true },
          { kRam, &g_hRam, &g_eRam, 100, true, true } },
        { { kGpu, &g_hGpu, &g_eGpu, 100, true, g_stats.gpu >= 0 },
          { kDisk, &g_hDisk, &g_eDisk, 100, true, g_stats.disk >= 0 } },
    };
    float x = sg.pad * S;
    for (int c = 0; c < 1 + SubtleColumns(); ++c) {
        float cw = (c == 0 ? sg.netW : sg.metW) * S;
        for (int r = 0; r < 2; ++r) {
            const Cell& k = cols[c][r];
            G::RectF cell(x, top + r * rowH, cw, rowH);
            if (k.available) {
                G::RectF gr(cell.X, cell.Y + 3 * S, cell.Width, cell.Height - 3.5f * S);
                g.SetClip(gr);
                DrawGraph(g, gr, *k.hist, k.e->shown, k.scale, ItemColor(k.item), step, phase, true, 1.0f * S, 95, 55);
                g.ResetClip();
            }
            wchar_t ic[2] = { IconFor(k.item), 0 }, val[32];
            if (!k.available) wcscpy(val, L"--");
            else if (k.pct) PctText(val, k.e->shown);
            else wcscpy(val, r == 0 ? up : down);
            bool alert = k.pct && k.available && k.e->target >= 90;
            Text(g, ic, g_fs.subtleIcon, cell, ItemColor(k.item), G::StringAlignmentNear, false);
            Text(g, val, g_fs.subtle, cell, alert ? LoadColor(100) : TextColor(), G::StringAlignmentFar, true);
        }
        x += cw + sg.gap * S;
    }
}

// TrafficMonitor-style compact grid: colored label, value, thin usage bar.
static void DrawClassicCell(G::Graphics& g, const G::RectF& r, Item item, const wchar_t* label, const wchar_t* value,
                            COLORREF valueColor, float pct, float S) {
    Text(g, label, g_fs.classic, r, ItemColor(item), G::StringAlignmentNear, false);
    Text(g, value, g_fs.classic, r, valueColor, G::StringAlignmentFar, false);
    if (pct >= 0) {
        float bh = std::max(1.0f, 2 * S);
        G::SolidBrush track(Col(TrackColor()));
        g.FillRectangle(&track, r.X, r.GetBottom() - bh, r.Width, bh);
        G::SolidBrush fill(Col(ItemColor(item)));
        g.FillRectangle(&fill, r.X, r.GetBottom() - bh, r.Width * std::min(pct, 100.0f) / 100, bh);
    }
}

static void DrawClassic(G::Graphics& g, int w, int h, int dpi) {
    float S = dpi / 96.0f;
    Layout L = Measure(dpi);
    bool vertical = h > w;
    wchar_t up[32], down[32], cpu[16], ram[16], gpu[16], disk[16];
    FormatRate(up, 32, g_eUp.shown);
    FormatRate(down, 32, g_eDown.shown);
    PctText(cpu, g_eCpu.shown);
    PctText(ram, g_eRam.shown);
    if (g_stats.gpu >= 0) PctText(gpu, g_eGpu.shown); else wcscpy(gpu, L"--");
    if (g_stats.disk >= 0) PctText(disk, g_eDisk.shown); else wcscpy(disk, L"--");

    struct Cell { Item item; const wchar_t* label; const wchar_t* value; COLORREF color; float pct; };
    Cell cells[] = {
        { kUp, L"↑", up, TextColor(), -1 },
        { kDown, L"↓", down, TextColor(), -1 },
        { kCpu, L"CPU", cpu, LoadColor(g_stats.cpu), g_eCpu.shown },
        { kRam, L"RAM", ram, LoadColor(g_stats.ram), g_eRam.shown },
        { kGpu, L"GPU", gpu, LoadColor(g_stats.gpu), g_stats.gpu >= 0 ? g_eGpu.shown : 0 },
        { kDisk, L"DISK", disk, LoadColor(g_stats.disk), g_stats.disk >= 0 ? g_eDisk.shown : 0 },
    };
    int nCells = g_set.extras ? 6 : 4;
    if (!vertical) {
        float top = (h - L.rowH * 2) / 2.0f;
        float colX[3] = { float(L.pad), float(L.pad + L.colNet + L.gap), float(L.pad + L.colNet + L.gap + L.colPct + L.gap) };
        float colW[3] = { float(L.colNet), float(L.colPct), float(L.colPct) };
        for (int i = 0; i < nCells; ++i) {
            int c = i / 2, row = i % 2;
            const Cell& k = cells[i];
            DrawClassicCell(g, G::RectF(colX[c], top + row * L.rowH, colW[c], float(L.rowH)), k.item, k.label, k.value, k.color, k.pct, S);
        }
    } else {
        float cw = float(w - 2 * L.pad);
        for (int i = 0; i < nCells; ++i) {
            const Cell& k = cells[i];
            DrawClassicCell(g, G::RectF(float(L.pad), float(L.pad + i * L.rowH), cw, float(L.rowH)), k.item, k.label, k.value, k.color, k.pct, S);
        }
    }
}

static void DrawCharacter(G::Graphics& g, const G::RectF& slot, float S) {
    if (g_char.frames.empty()) return;
    G::Bitmap* f = g_char.frames[g_char.frame % g_char.frames.size()];
    CharSize cs = CharacterSize(S);
    G::RectF dst(std::round(slot.X + 3 * S), std::round(slot.Y + (slot.Height - cs.h) / 2), cs.w, cs.h);
    g.SetInterpolationMode(cs.nearest ? G::InterpolationModeNearestNeighbor : G::InterpolationModeHighQualityBicubic);
    if (g_char.tint) {                          // black silhouette -> theme text color
        COLORREF c = TextColor();
        G::ColorMatrix cm = { { { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0.92f, 0 },
                                { GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 0, 1 } } };
        G::ImageAttributes attr;
        attr.SetColorMatrix(&cm);
        g.DrawImage(f, dst, 0, 0, float(f->GetWidth()), float(f->GetHeight()), G::UnitPixel, &attr);
    } else {
        g.DrawImage(f, dst);
    }
}

// ---- Frame ------------------------------------------------------------------------------------
// GDI+ draws straight into a premultiplied 32-bit DIB, which UpdateLayeredWindow pushes to the
// taskbar (TrafficMonitor imports the same API family). Empty pixels get alpha 1: invisible, but
// unlike a color key they still receive the mouse, so hover/right-click work anywhere.
static HDC g_memDC = nullptr;
static HBITMAP g_dib = nullptr, g_dibOld = nullptr;
static void* g_bits = nullptr;
static int g_dibW = 0, g_dibH = 0;

static void FreeFrame() {
    if (g_memDC && g_dibOld) SelectObject(g_memDC, g_dibOld);
    if (g_dib) DeleteObject(g_dib);
    if (g_memDC) DeleteDC(g_memDC);
    g_memDC = nullptr; g_dib = g_dibOld = nullptr; g_bits = nullptr; g_dibW = g_dibH = 0;
}

static bool EnsureFrame(int w, int h) {
    if (g_dib && w == g_dibW && h == g_dibH) return true;
    FreeFrame();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                 // top-down, matches GDI+ scan order
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_memDC = CreateCompatibleDC(nullptr);
    g_dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    if (!g_memDC || !g_dib) { FreeFrame(); return false; }
    g_dibOld = (HBITMAP)SelectObject(g_memDC, g_dib);
    g_dibW = w; g_dibH = h;
    return true;
}

static void Render() {
    if (!g_widget) return;
    RECT rc;
    GetClientRect(g_widget, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0 || !EnsureFrame(w, h)) return;
    int dpi = DpiOf(g_widget);
    float S = dpi / 96.0f;
    EnsureFonts(dpi);
    {
        G::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)g_bits);
        G::Graphics g(&canvas);
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(G::TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
        g.Clear(G::Color(1, 0, 0, 0));
        bool vertical = h > w;
        if (!vertical) {
            EnsureBackground(w, h, S);
            if (g_bgCache) g.DrawImage(g_bgCache, 0, 0, w, h);
            if (float slot = CharSlotW(S) * S) {
                DrawCharacter(g, G::RectF(0, 0, slot, float(h)), S);
                g.TranslateTransform(slot, 0);  // the chosen style draws to the right of it
            }
        }
        if (vertical || g_set.style == kStyleClassic) DrawClassic(g, w, h, dpi);
        else if (g_set.style == kStyleSubtle) DrawSubtle(g, h, S);
        else DrawModern(g, h, S, g_set.style == kStyleRings);
    }
    SIZE size{ w, h };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_widget, nullptr, nullptr, &size, g_memDC, &src, 0, &bf, ULW_ALPHA);
}

static void UpdateFrameTimer() {
    bool running = g_set.character != kCharNone && (EnsureCharacter(), g_char.frames.size() > 1);
    if (g_set.animate || running) SetTimer(g_ctrl, kFrameTimerId, kFrameMs, nullptr);
    else KillTimer(g_ctrl, kFrameTimerId);
}

// ---------------------------------------------------------------------------------------------
// Today's traffic (persisted so restarts within the day keep counting)
// ---------------------------------------------------------------------------------------------
static NetSampler* g_net = nullptr;
static DWORD g_today = 0;

static DWORD TodayStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 10000u + st.wMonth * 100u + st.wDay;
}
static void LoadToday() {
    g_today = TodayStamp();
    if (RegReadDword(L"TodayDate", 0) == g_today) {
        g_net->todayIn = RegReadQword(L"TodayIn");
        g_net->todayOut = RegReadQword(L"TodayOut");
    }
}
static void SaveToday() {
    RegWrite(L"TodayDate", REG_DWORD, &g_today, sizeof(DWORD));
    RegWrite(L"TodayIn", REG_QWORD, &g_net->todayIn, sizeof(ULONG64));
    RegWrite(L"TodayOut", REG_QWORD, &g_net->todayOut, sizeof(ULONG64));
}
static void RollDay() {
    DWORD now = TodayStamp();
    if (now != g_today) { g_today = now; g_net->todayIn = g_net->todayOut = 0; SaveToday(); }
}

static void UpdateTooltip() {
    if (!g_tip) return;
    const Stats& s = g_stats;
    wchar_t total[32], in[32], out[32], used[32], all[32], up[32], down[32], link[32];
    FormatBytes(total, 32, g_net->todayIn + g_net->todayOut);
    FormatBytes(in, 32, g_net->todayIn);
    FormatBytes(out, 32, g_net->todayOut);
    FormatBytes(used, 32, s.ramUsed);
    FormatBytes(all, 32, s.ramTotal);
    FormatRate(up, 32, s.upBps);
    FormatRate(down, 32, s.downBps);
    if (s.linkBps >= 1000000) swprintf(link, 32, L"%llu Mbps", s.linkBps / 1000000ULL);
    else wcscpy(link, L"n/a");

    swprintf(g_tipText, 1024,
             L"Connection: %ls\nLink rate: %ls (the most the Wi-Fi/Ethernet link can carry)\n"
             L"Upload: %ls    Download: %ls\n"
             L"Traffic today: %ls (up %ls / down %ls)\n"
             L"CPU usage: %d%%\nMemory: %ls / %ls (%d%%)\nGPU usage: %ls\nDisk active time: %ls",
             s.adapter.empty() ? L"none" : s.adapter.c_str(), link, up, down, total, out, in,
             s.cpu, used, all, s.ram,
             s.gpu >= 0 ? (std::to_wstring(s.gpu) + L"%").c_str() : L"n/a",
             s.disk >= 0 ? (std::to_wstring(s.disk) + L"%").c_str() : L"n/a");
    if (g_tipShown) PlaceTooltip();
}

// ---------------------------------------------------------------------------------------------
// Window procedures
// ---------------------------------------------------------------------------------------------
static void Relayout() {
    RestoreTaskList();
    if (g_widget) { Reposition(); Render(); }
}

// <exe folder>\characters: every *.gif in it shows up in the Character menu.
static std::wstring CharactersDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    return std::wstring(exe) + L"\\characters";
}

static std::vector<std::wstring> g_libGifs;

static std::vector<std::wstring> ListCharacterGifs() {
    std::vector<std::wstring> out;
    std::wstring dir = CharactersDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.gif").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

// "black-cat.gif" -> "Black cat"
static std::wstring GifLabel(const std::wstring& path) {
    std::wstring name = PathFindFileNameW(path.c_str());
    size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    for (wchar_t& c : name) if (c == L'-' || c == L'_') c = L' ';
    if (!name.empty()) name[0] = towupper(name[0]);
    return name;
}

static void SetCharacter(DWORD mode, const std::wstring& gif = L"") {
    g_set.character = mode;
    if (mode == kCharGif) g_set.characterGif = gif;
    SaveSettings();
    UpdateFrameTimer();
    Relayout();
}

static void ShowMenu() {
    HMENU pos = CreatePopupMenu();
    AppendMenuW(pos, MF_STRING | (g_set.posRight ? 0 : MF_CHECKED), kIdPosLeft, L"Left of taskbar");
    AppendMenuW(pos, MF_STRING | (g_set.posRight ? MF_CHECKED : 0), kIdPosRight, L"Next to the tray");
    HMENU units = CreatePopupMenu();
    AppendMenuW(units, MF_STRING | (g_set.bits ? MF_CHECKED : 0), kIdBits, L"Bits per second (Mbps, like speed tests)");
    AppendMenuW(units, MF_STRING | (g_set.bits ? 0 : MF_CHECKED), kIdBytes, L"Bytes per second (MB/s, like downloads)");

    HMENU style = CreatePopupMenu();
    AppendMenuW(style, MF_STRING | (g_set.style == kStyleSubtle ? MF_CHECKED : 0), kIdStyleSubtle, L"Subtle");
    AppendMenuW(style, MF_STRING | (g_set.style == kStyleCards ? MF_CHECKED : 0), kIdStyleCards, L"Cards with live graphs");
    AppendMenuW(style, MF_STRING | (g_set.style == kStyleRings ? MF_CHECKED : 0), kIdStyleRings, L"Rings");
    AppendMenuW(style, MF_STRING | (g_set.style == kStyleClassic ? MF_CHECKED : 0), kIdStyleClassic, L"Classic text");
    HMENU bg = CreatePopupMenu();
    AppendMenuW(bg, MF_STRING, kIdBgChoose, L"Choose image...");
    AppendMenuW(bg, MF_STRING | (g_set.bgImage.empty() ? MF_GRAYED : 0), kIdBgRemove, L"Remove image");
    AppendMenuW(bg, MF_SEPARATOR, 0, nullptr);
    const struct { UINT id; DWORD pct; const wchar_t* text; } ops[] = {
        { kIdOpacity10, 10, L"Opacity 10%" }, { kIdOpacity20, 20, L"Opacity 20%" },
        { kIdOpacity35, 35, L"Opacity 35%" }, { kIdOpacity50, 50, L"Opacity 50%" },
    };
    for (auto& o : ops)
        AppendMenuW(bg, MF_STRING | (g_set.bgOpacity == o.pct ? MF_CHECKED : 0) | (g_set.bgImage.empty() ? MF_GRAYED : 0), o.id, o.text);

    HMENU chr = CreatePopupMenu();
    AppendMenuW(chr, MF_STRING | (g_set.character == kCharCat ? MF_CHECKED : 0), kIdCharCat, L"Cat");
    AppendMenuW(chr, MF_STRING | (g_set.character == kCharParrot ? MF_CHECKED : 0), kIdCharParrot, L"Parrot");
    AppendMenuW(chr, MF_STRING | (g_set.character == kCharHorse ? MF_CHECKED : 0), kIdCharHorse, L"Horse");
    g_libGifs = ListCharacterGifs();
    if (!g_libGifs.empty()) AppendMenuW(chr, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < g_libGifs.size() && i < 200; ++i) {
        bool on = g_set.character == kCharGif && _wcsicmp(g_set.characterGif.c_str(), g_libGifs[i].c_str()) == 0;
        AppendMenuW(chr, MF_STRING | (on ? MF_CHECKED : 0), kIdCharLibBase + UINT(i), GifLabel(g_libGifs[i]).c_str());
    }
    AppendMenuW(chr, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(chr, MF_STRING, kIdCharGif, L"Choose a GIF...");
    AppendMenuW(chr, MF_STRING, kIdCharFolder, L"Open characters folder");
    HMENU src = CreatePopupMenu();
    AppendMenuW(src, MF_STRING | (g_set.speedSource == kSrcCpu ? MF_CHECKED : 0), kIdSrcCpu, L"CPU usage");
    AppendMenuW(src, MF_STRING | (g_set.speedSource == kSrcRam ? MF_CHECKED : 0), kIdSrcRam, L"RAM usage");
    AppendMenuW(src, MF_STRING | (g_set.speedSource == kSrcNet ? MF_CHECKED : 0), kIdSrcNet, L"Network speed");
    AppendMenuW(chr, MF_POPUP, (UINT_PTR)src, L"Runs faster with");
    AppendMenuW(chr, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(chr, MF_STRING | (g_set.character == kCharNone ? MF_CHECKED : 0), kIdCharNone, L"None");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_POPUP, (UINT_PTR)style, L"Style");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)chr, L"Character");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)bg, L"Background image");
    AppendMenuW(m, MF_STRING | (g_set.animate ? MF_CHECKED : 0), kIdAnimate, L"Animations");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pos, L"Position");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)units, L"Speed units");
    AppendMenuW(m, MF_STRING | (g_set.extras ? MF_CHECKED : 0), kIdExtras, L"Show GPU and disk");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), kIdStartup, L"Start with Windows");
    AppendMenuW(m, MF_STRING, kIdResetToday, L"Reset today's traffic");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kIdTaskMgr, L"Open Task Manager");
    AppendMenuW(m, MF_STRING, kIdExit, L"Exit TaskMon");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_ctrl);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_ctrl, nullptr);
    DestroyMenu(m);                             // also destroys the submenus

    switch (cmd) {
    case kIdPosLeft:   g_set.posRight = 0; SaveSettings(); Relayout(); break;
    case kIdPosRight:  g_set.posRight = 1; SaveSettings(); Relayout(); break;
    case kIdBits:      g_set.bits = 1; SaveSettings(); Relayout(); break;
    case kIdBytes:     g_set.bits = 0; SaveSettings(); Relayout(); break;
    case kIdExtras:    g_set.extras = !g_set.extras; SaveSettings(); Relayout(); break;
    case kIdStyleSubtle:  g_set.style = kStyleSubtle; SaveSettings(); Relayout(); break;
    case kIdStyleCards:   g_set.style = kStyleCards; SaveSettings(); Relayout(); break;
    case kIdStyleRings:   g_set.style = kStyleRings; SaveSettings(); Relayout(); break;
    case kIdStyleClassic: g_set.style = kStyleClassic; SaveSettings(); Relayout(); break;
    case kIdAnimate:
        g_set.animate = !g_set.animate;
        SaveSettings();
        if (!g_set.animate) for (Eased* e : g_allEased) e->Snap();
        UpdateFrameTimer();
        Render();
        break;
    case kIdBgChoose: {
        wchar_t file[MAX_PATH * 2] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_ctrl;
        ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"Choose a background image for TaskMon";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            g_set.bgImage = file;
            SaveSettings();
            g_bgLoaded.clear();
            Render();
        }
        break;
    }
    case kIdCharNone:   SetCharacter(kCharNone); break;
    case kIdCharCat:    SetCharacter(kCharCat); break;
    case kIdCharParrot: SetCharacter(kCharParrot); break;
    case kIdCharHorse:  SetCharacter(kCharHorse); break;
    case kIdSrcCpu: g_set.speedSource = kSrcCpu; SaveSettings(); break;
    case kIdSrcRam: g_set.speedSource = kSrcRam; SaveSettings(); break;
    case kIdSrcNet: g_set.speedSource = kSrcNet; SaveSettings(); break;
    case kIdCharFolder: {
        std::wstring dir = CharactersDir();
        CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case kIdCharGif: {
        wchar_t file[MAX_PATH * 2] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_ctrl;
        ofn.lpstrFilter = L"Animated GIF (*.gif)\0*.gif\0Images (*.png;*.gif)\0*.png;*.gif\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"Choose a character GIF for TaskMon";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) SetCharacter(kCharGif, file);
        break;
    }
    default:
        if (cmd >= kIdCharLibBase && cmd < kIdCharLibBase + g_libGifs.size())
            SetCharacter(kCharGif, g_libGifs[cmd - kIdCharLibBase]);
        break;
    case kIdBgRemove:  g_set.bgImage.clear(); SaveSettings(); Render(); break;
    case kIdOpacity10: g_set.bgOpacity = 10; SaveSettings(); Render(); break;
    case kIdOpacity20: g_set.bgOpacity = 20; SaveSettings(); Render(); break;
    case kIdOpacity35: g_set.bgOpacity = 35; SaveSettings(); Render(); break;
    case kIdOpacity50: g_set.bgOpacity = 50; SaveSettings(); Render(); break;
    case kIdStartup:   SetStartup(!StartupEnabled()); break;
    case kIdResetToday: g_net->todayIn = g_net->todayOut = 0; SaveToday(); break;
    case kIdTaskMgr:   ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL); break;
    case kIdExit:      PostMessageW(g_ctrl, WM_CLOSE, 0, 0); break;
    }
}

static LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: ValidateRect(hwnd, nullptr); return 0;   // content comes from UpdateLayeredWindow
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    // Explorer floods taskbar children with synthetic WM_MOUSEMOVEs, which keeps resetting
    // TrackMouseEvent's hover timer, so hover is detected with our own timer + cursor check.
    case WM_MOUSEMOVE:
        if (!g_mouseTracked) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_mouseTracked = TrackMouseEvent(&tme) != FALSE;
            if (!g_tipShown) SetTimer(hwnd, kHoverTimerId, 500, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wp == kHoverTimerId) {
            KillTimer(hwnd, kHoverTimerId);
            if (CursorOverWidget()) { UpdateTooltip(); ShowTooltip(true); }
        }
        return 0;
    case WM_MOUSELEAVE:
        g_mouseTracked = false;
        if (!CursorOverWidget()) { KillTimer(hwnd, kHoverTimerId); ShowTooltip(false); }
        return 0;
    case WM_RBUTTONUP: ShowTooltip(false); ShowMenu(); return 0;
    case WM_LBUTTONDBLCLK: ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL); return 0;
    case WM_NCHITTEST: return HTCLIENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static CpuSampler* g_cpu = nullptr;
static GpuDiskSampler* g_gpuDisk = nullptr;
static int g_tick = 0;

static void OnTick() {
    RollDay();
    g_net->Sample(g_stats);
    g_stats.cpu = g_cpu->Sample();
    SampleRam(g_stats);
    if (g_set.extras) g_gpuDisk->Sample(g_stats.gpu, g_stats.disk);
    PushSamples();

    // Re-attach if Explorer restarted or our parent vanished; otherwise keep our slot.
    bool ok = g_widget && IsWindow(g_widget) && IsWindow(g_dock.parent) &&
              FindWindowW(L"Shell_TrayWnd", nullptr) == g_dock.tray;
    if (!ok) ok = Attach();
    else ok = Reposition();

    ++g_tick;
    if (g_tick % 5 == 0) g_light = ReadLightTheme();
    if (g_tick % 60 == 0) SaveToday();
    if (g_tipShown && !CursorOverWidget()) ShowTooltip(false);   // safety net for a missed leave
    if (ok && g_widget) {
        if (!g_set.animate) Render();           // otherwise the frame timer is already drawing
        UpdateTooltip();
    }
}

static LRESULT CALLBACK CtrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        g_widget = nullptr;                     // old one died with the previous Explorer
        Attach();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == kTimerId) OnTick();
        else if (wp == kFrameTimerId && g_widget) { StepAnimation(); Render(); }
        return 0;
    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        g_light = ReadLightTheme();
        if (g_widget) { Reposition(); Render(); }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) { SaveToday(); Detach(); }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        KillTimer(hwnd, kFrameTimerId);
        SaveToday();
        Detach();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnableDpiAwareness() {
    typedef BOOL(WINAPI * Fn)(HANDLE);
    Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (fn && fn((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    // "--startup on|off" lets an installer toggle the Run entry without showing the widget.
    if (cmdLine && wcsstr(cmdLine, L"--startup")) {
        SetStartup(wcsstr(cmdLine, L"off") == nullptr);
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\TaskMonSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    EnableDpiAwareness();
    LoadSettings();
    G::GdiplusStartupInput gdipIn;
    ULONG_PTR gdipToken = 0;
    G::GdiplusStartup(&gdipToken, &gdipIn, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = CtrlProc;
    wc.lpszClassName = kCtrlClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = WidgetProc;
    wc.lpszClassName = kWidgetClass;
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);

    // A hidden top-level (not message-only) window, so it receives the TaskbarCreated broadcast.
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_ctrl = CreateWindowExW(WS_EX_TOOLWINDOW, kCtrlClass, L"TaskMon", WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, inst, nullptr);
    if (!g_ctrl) return 1;
    CreateTipWindow(inst);

    CpuSampler cpu;
    NetSampler net;
    GpuDiskSampler gpuDisk;
    g_cpu = &cpu;
    g_net = &net;
    g_gpuDisk = &gpuDisk;
    LoadToday();
    SampleRam(g_stats);
    net.Sample(g_stats);
    g_stats.cpu = cpu.Sample();
    PushSamples();
    g_lastFrame = GetTickCount64();

    InitializeCriticalSection(&g_btnLock);
    HANDLE scanner = CreateThread(nullptr, 0, ButtonScanThread, nullptr, 0, nullptr);

    Attach();
    SetTimer(g_ctrl, kTimerId, 1000, nullptr);
    UpdateFrameTimer();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    InterlockedExchange(&g_quit, 1);
    if (scanner) { WaitForSingleObject(scanner, 3000); CloseHandle(scanner); }
    if (g_font) DeleteObject(g_font);
    FreeFrame();
    g_fs.Free();
    g_char.Clear();
    InvalidateBackground();
    delete g_bgSrc;
    G::GdiplusShutdown(gdipToken);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
