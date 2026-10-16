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
// Settings live in HKCU\Software\TaskMon; "Start with Windows" uses HKCU\...\Run.
//
// Build (MSYS2 UCRT64 g++):
//   g++ -O2 -s -std=c++17 -municode -mwindows -static taskmon.cpp -o TaskMon.exe -liphlpapi -lpdh -lole32 -loleaut32
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
#include <uiautomation.h>
#include <cstdio>
#include <cwchar>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

static const wchar_t kCtrlClass[]   = L"TaskMonController";
static const wchar_t kWidgetClass[] = L"TaskMonWidget";
static const wchar_t kRegKey[]      = L"Software\\TaskMon";
static const wchar_t kRunKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[]    = L"TaskMon";
static const UINT_PTR kTimerId = 1;

enum : UINT {
    kIdExit = 100, kIdTaskMgr, kIdPosLeft, kIdPosRight, kIdBits, kIdBytes, kIdExtras, kIdStartup, kIdResetToday,
};

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
struct Settings {
    DWORD posRight = 0;     // 0 = far left of the taskbar, 1 = next to the tray
    DWORD bits = 1;         // 1 = Mbps (what speed tests/ISPs quote), 0 = MB/s
    DWORD extras = 1;       // show GPU + disk column
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
static void LoadSettings() {
    g_set.posRight = RegReadDword(L"PositionRight", 0);
    g_set.bits = RegReadDword(L"SpeedInBits", 1);
    g_set.extras = RegReadDword(L"ShowGpuDisk", 1);
}
static void SaveSettings() {
    RegWrite(L"PositionRight", REG_DWORD, &g_set.posRight, sizeof(DWORD));
    RegWrite(L"SpeedInBits", REG_DWORD, &g_set.bits, sizeof(DWORD));
    RegWrite(L"ShowGpuDisk", REG_DWORD, &g_set.extras, sizeof(DWORD));
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

static int WidgetWidth(const Layout& L) {
    int w = L.pad + L.colNet + L.gap + L.colPct + L.pad;
    if (g_set.extras) w += L.gap + L.colPct;
    return w;
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

    int w = vertical ? pw : WidgetWidth(L);
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

// One "LABEL  value" cell; percentage cells also get a thin usage bar along the bottom.
// In the mask pass everything is drawn white so the result is pure coverage (alpha).
static void DrawCell(HDC dc, bool mask, int x, int y, int w, int h, Item item, const wchar_t* label,
                     const wchar_t* value, COLORREF valueColor, int pct, int dpi) {
    const COLORREF white = RGB(255, 255, 255);
    RECT r{ x, y, x + w, y + h };
    SetTextColor(dc, mask ? white : ItemColor(item));
    DrawTextW(dc, label, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, mask ? white : valueColor);
    DrawTextW(dc, value, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (pct >= 0) {
        int bh = std::max(1, MulDiv(2, dpi, 96));
        RECT track{ x, y + h - bh, x + w, y + h };
        HBRUSH tb = CreateSolidBrush(mask ? white : TrackColor());
        FillRect(dc, &track, tb);
        DeleteObject(tb);
        RECT fill{ x, track.top, x + MulDiv(w, std::min(pct, 100), 100), track.bottom };
        HBRUSH fb = CreateSolidBrush(mask ? white : ItemColor(item));
        FillRect(dc, &fill, fb);
        DeleteObject(fb);
    }
}

static void DrawScene(HDC mem, bool mask, int w, int h, int dpi) {
    RECT rc{ 0, 0, w, h };
    FillRect(mem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    bool vertical = h > w;
    Layout L = Measure(dpi);
    HGDIOBJ oldFont = SelectObject(mem, g_font);
    SetBkMode(mem, TRANSPARENT);

    const Stats& s = g_stats;
    wchar_t up[32], down[32], cpu[16], ram[16], gpu[16], disk[16];
    FormatRate(up, 32, s.upBps);
    FormatRate(down, 32, s.downBps);
    swprintf(cpu, 16, L"%d%%", s.cpu);
    swprintf(ram, 16, L"%d%%", s.ram);
    if (s.gpu >= 0) swprintf(gpu, 16, L"%d%%", s.gpu); else wcscpy(gpu, L"--");
    if (s.disk >= 0) swprintf(disk, 16, L"%d%%", s.disk); else wcscpy(disk, L"--");

    struct Cell { Item item; const wchar_t* label; const wchar_t* value; COLORREF color; int pct; };
    Cell cells[] = {
        { kUp, L"↑", up, TextColor(), -1 },
        { kDown, L"↓", down, TextColor(), -1 },
        { kCpu, L"CPU", cpu, LoadColor(s.cpu), s.cpu },
        { kRam, L"RAM", ram, LoadColor(s.ram), s.ram },
        { kGpu, L"GPU", gpu, LoadColor(s.gpu), s.gpu },
        { kDisk, L"DISK", disk, LoadColor(s.disk), s.disk },
    };
    int nCells = g_set.extras ? 6 : 4;

    if (!vertical) {
        int top = (h - L.rowH * 2) / 2;
        int colX[3] = { L.pad, L.pad + L.colNet + L.gap, L.pad + L.colNet + L.gap + L.colPct + L.gap };
        int colW[3] = { L.colNet, L.colPct, L.colPct };
        for (int i = 0; i < nCells; ++i) {
            int c = i / 2, row = i % 2;
            const Cell& k = cells[i];
            DrawCell(mem, mask, colX[c], top + row * L.rowH, colW[c], L.rowH, k.item, k.label, k.value, k.color, k.pct, dpi);
        }
    } else {
        int cw = w - 2 * L.pad;
        for (int i = 0; i < nCells; ++i) {
            const Cell& k = cells[i];
            DrawCell(mem, mask, L.pad, L.pad + i * L.rowH, cw, L.rowH, k.item, k.label, k.value, k.color, k.pct, dpi);
        }
    }
    SelectObject(mem, oldFont);
}

// Per-pixel-alpha rendering via UpdateLayeredWindow (TrafficMonitor imports the same family).
// The scene is drawn twice on black: in color (which is already premultiplied by coverage) and
// as a white mask (coverage = alpha). Background pixels get alpha 1: invisible, but unlike a
// color key they still receive the mouse, so hover/right-click work anywhere on the widget.
static void Render() {
    if (!g_widget) return;
    RECT rc;
    GetClientRect(g_widget, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                 // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *colorBits = nullptr, *maskBits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &colorBits, nullptr, 0);
    HBITMAP maskBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
    HDC mem = CreateCompatibleDC(nullptr);
    if (colorBmp && maskBmp && mem) {
        int dpi = DpiOf(g_widget);
        HGDIOBJ old = SelectObject(mem, maskBmp);
        DrawScene(mem, true, w, h, dpi);
        SelectObject(mem, colorBmp);
        DrawScene(mem, false, w, h, dpi);
        GdiFlush();

        BYTE* c = (BYTE*)colorBits;
        const BYTE* m = (const BYTE*)maskBits;
        for (int i = 0; i < w * h; ++i, c += 4, m += 4) {
            BYTE a = std::max({ m[0], m[1], m[2], BYTE(1) });
            c[0] = std::min(c[0], a);
            c[1] = std::min(c[1], a);
            c[2] = std::min(c[2], a);
            c[3] = a;
        }
        SIZE size{ w, h };
        POINT src{ 0, 0 };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_widget, nullptr, nullptr, &size, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (mem) DeleteDC(mem);
    if (colorBmp) DeleteObject(colorBmp);
    if (maskBmp) DeleteObject(maskBmp);
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

static void ShowMenu() {
    HMENU pos = CreatePopupMenu();
    AppendMenuW(pos, MF_STRING | (g_set.posRight ? 0 : MF_CHECKED), kIdPosLeft, L"Left of taskbar");
    AppendMenuW(pos, MF_STRING | (g_set.posRight ? MF_CHECKED : 0), kIdPosRight, L"Next to the tray");
    HMENU units = CreatePopupMenu();
    AppendMenuW(units, MF_STRING | (g_set.bits ? MF_CHECKED : 0), kIdBits, L"Bits per second (Mbps, like speed tests)");
    AppendMenuW(units, MF_STRING | (g_set.bits ? 0 : MF_CHECKED), kIdBytes, L"Bytes per second (MB/s, like downloads)");

    HMENU m = CreatePopupMenu();
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

    // Re-attach if Explorer restarted or our parent vanished; otherwise keep our slot.
    bool ok = g_widget && IsWindow(g_widget) && IsWindow(g_dock.parent) &&
              FindWindowW(L"Shell_TrayWnd", nullptr) == g_dock.tray;
    if (!ok) ok = Attach();
    else ok = Reposition();

    ++g_tick;
    if (g_tick % 5 == 0) g_light = ReadLightTheme();
    if (g_tick % 60 == 0) SaveToday();
    if (g_tipShown && !CursorOverWidget()) ShowTooltip(false);   // safety net for a missed leave
    if (ok && g_widget) { Render(); UpdateTooltip(); }
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

    InitializeCriticalSection(&g_btnLock);
    HANDLE scanner = CreateThread(nullptr, 0, ButtonScanThread, nullptr, 0, nullptr);

    Attach();
    SetTimer(g_ctrl, kTimerId, 1000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    InterlockedExchange(&g_quit, 1);
    if (scanner) { WaitForSingleObject(scanner, 3000); CloseHandle(scanner); }
    if (g_font) DeleteObject(g_font);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
