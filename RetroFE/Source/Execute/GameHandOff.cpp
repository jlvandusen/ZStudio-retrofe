/* This file is part of RetroFE (ZStudio ArcadeMaster fork).
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
// See GameHandOff.h. Ported from ArcadeMaster's AmPanel (src/panel/LoadingScreen.cs), where each step was found on a
// cabinet:
//  - keep on the main screen: Windows opens a new program on the screen of the last touch, so a game could open on the
//    touch screen (Pac-Man Battle Royale, 2026-11-26): a big new window on another screen moves to the main screen,
//    same place, same size
//  - bring forward: a game could run BEHIND the frontend and take no input (Star Wars Trilogy, 2026-11-26): while the
//    frontend, the shell or nothing has the foreground, the game window gets it
//  - show the game: Supermodel draws nothing full screen until its window loses the focus and gets it back (what Alt+Tab
//    did, 2026-11-27): 3 s after a near-full-screen game window is up on the main screen, the frontend's main-screen
//    windows are minimised, the focus goes to the frontend's touch-screen window for 400 ms, then to the game
//  - emulator consoles: on Windows 11 a console program's window opens in Windows Terminal, which a launcher cannot
//    hide (Supermodel under RetroBat): those windows are minimised
//  - rectangles are read per-monitor DPI aware: on a 4K screen at 200 % a DPI-unaware read is virtualised (pedestal)
#ifdef WIN32
#include "GameHandOff.h"
#include "../Utility/Log.h"
#include <algorithm>

namespace {
// programs whose windows are never "the game": the frontend, ArcadeMaster's panel, the shell, launchers' own small windows
const wchar_t* kNotTheGame[] = {
    L"retrofe", L"AmPanel", L"LEDBlinky", L"explorer", L"TeknoParrotUi", L"emulatorLauncher", L"cmd", L"conhost",
    L"powershell", L"WindowsTerminal", L"OpenConsole", L"pwsh", L"RustDesk", L"msedge", L"chrome", L"SnippingTool",
    L"ScreenClippingHost", L"ScreenSketch", L"GameBar", L"Taskmgr", L"wscript", L"dwm", L"ShellExperienceHost",
    L"SearchHost", L"StartMenuExperienceHost", L"TextInputHost", L"Code", L"Claude",
    // TeknoParrot's loaders put up their own windows before the game is ready
    L"OpenParrotLoader", L"OpenParrotLoader64", L"OpenParrotKonamiLoader", L"BudgieLoader", L"ElfLdr2", L"TeknoParrot",
    L"ParrotLoader" };

std::wstring processName(DWORD pid) {
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return name;
    wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        name.assign(buf, n);
        size_t slash = name.find_last_of(L"\\/"); if (slash != std::wstring::npos) name = name.substr(slash + 1);
        size_t dot = name.find_last_of(L'.'); if (dot != std::wstring::npos) name = name.substr(0, dot);
    }
    CloseHandle(h);
    return name;
}

bool iequals(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

bool notTheGame(const std::wstring& name) {
    for (auto* n : kNotTheGame) if (iequals(name, n)) return true;
    return false;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// per-monitor DPI awareness for this thread while we read window and monitor rectangles (Windows 10 1703+; the call is
// looked up so older systems simply keep the process default)
struct DpiScope {
    using SetCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetCtx set = nullptr; DPI_AWARENESS_CONTEXT old = nullptr;
    DpiScope() {
        set = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
        if (set) old = set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~DpiScope() { if (set && old) set(old); }
};

// an Alt tap: Windows lets the process that sent the last input hand over the foreground
void altTap() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}
}

GameHandOff::GameHandOff(bool enabled) : enabled_(enabled) {
    start_ = lastTick_ = std::chrono::steady_clock::now();
    if (!enabled_) return;
    for (auto& w : bigWindows(0.5)) baseline_.insert(w.hwnd);
    // the pointer on the main screen: Windows opens a new program on the screen of the last touch / click
    POINT c{ GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2 };
    SetCursorPos(c.x, c.y);
    LOG_INFO("GameHandOff", "on: " + std::to_string(baseline_.size()) + " window(s) up before the launch");
}

GameHandOff::~GameHandOff() { if (enabled_) restoreFrontend(); }

std::vector<GameHandOff::Win> GameHandOff::bigWindows(double cover) {
    std::vector<Win> out;
    DpiScope dpi;
    struct Ctx { std::vector<Win>* out; double cover; DWORD me; } ctx{ &out, cover, GetCurrentProcessId() };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
        RECT r; if (!GetWindowRect(h, &r)) return TRUE;
        HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; if (!GetMonitorInfoW(m, &mi)) return TRUE;
        long w = r.right - r.left, hh = r.bottom - r.top;
        long mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        if (w < mw * c->cover || hh < mh * c->cover) return TRUE;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == c->me) return TRUE;
        std::wstring name = processName(pid);
        if (name.empty() || notTheGame(name)) return TRUE;
        c->out->push_back(Win{ h, pid, name, r, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

void GameHandOff::tick() {
    if (!enabled_) return;
    auto now = std::chrono::steady_clock::now();
    if (now - lastTick_ < std::chrono::milliseconds(400)) return;
    lastTick_ = now;
    if (now - start_ > std::chrono::seconds(120)) return;     // the game has long settled: hands off

    minimiseEmulatorConsoles();
    DpiScope dpi;
    MONITORINFO pm{ sizeof(pm) };
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &pm);
    const RECT& main = pm.rcMonitor;
    for (auto& w : bigWindows(0.4)) {
        if (baseline_.count(w.hwnd) || moved_.count(w.hwnd)) continue;
        if (!w.primary) {
            // same place on the main screen, same size (a borderless full-screen game stays full screen)
            int x = main.left + (w.rect.left - w.monitor.left), y = main.top + (w.rect.top - w.monitor.top);
            SetWindowPos(w.hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            moved_.insert(w.hwnd);
            LOG_INFO("GameHandOff", "game window " + narrow(w.process) + " opened on another screen - moved to the main screen");
            continue;
        }
        bringForward(w.hwnd);
        auto it = seen_.find(w.hwnd);
        if (it == seen_.end()) { seen_[w.hwnd] = now; continue; }
        long mw = main.right - main.left, mh = main.bottom - main.top;
        if (!shown_.count(w.hwnd) && now - it->second > std::chrono::seconds(3) &&
            (w.rect.right - w.rect.left) >= mw * 0.9 && (w.rect.bottom - w.rect.top) >= mh * 0.9) {
            shown_.insert(w.hwnd);
            showGame(w.hwnd);
        }
    }
}

void GameHandOff::minimiseEmulatorConsoles() {
    struct Ctx { GameHandOff* self; } ctx{ this };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* self = reinterpret_cast<Ctx*>(l)->self;
        if (self->consolesDone_.count(h) || !IsWindowVisible(h) || IsIconic(h)) return TRUE;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        std::wstring name = processName(pid);
        if (!iequals(name, L"WindowsTerminal") && !iequals(name, L"OpenConsole")) return TRUE;
        wchar_t t[512]; int n = GetWindowTextW(h, t, 512);
        std::wstring title(t, n > 0 ? n : 0), low = title;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        // only an emulator's console (its path or RetroBat in the title): never somebody's own terminal
        if (low.find(L"\\emulators\\") == std::wstring::npos && low.find(L"retrobat") == std::wstring::npos) return TRUE;
        ShowWindow(h, SW_MINIMIZE);
        self->consolesDone_.insert(h);
        LOG_INFO("GameHandOff", "emulator console '" + narrow(title) + "' minimised");
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

// the game in front with the keyboard - only while the frontend, the shell or nothing has the foreground, so a game's
// own launcher or dialog the player is using is never pushed aside
void GameHandOff::bringForward(HWND game) {
    HWND fg = GetForegroundWindow();
    if (fg == game) return;
    if (fg) {
        DWORD fgPid = 0; GetWindowThreadProcessId(fg, &fgPid);
        std::wstring fgName = fgPid == GetCurrentProcessId() ? L"retrofe" : processName(fgPid);
        if (!iequals(fgName, L"retrofe") && !iequals(fgName, L"explorer")) return;
    }
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    if (ft && ft != me) AttachThreadInput(me, ft, TRUE);
    ShowWindow(game, SW_RESTORE); BringWindowToTop(game); SetForegroundWindow(game);
    if (ft && ft != me) AttachThreadInput(me, ft, FALSE);
    if (broughtUp_.insert(game).second) {
        DWORD pid = 0; GetWindowThreadProcessId(game, &pid);
        LOG_INFO("GameHandOff", "game window " + narrow(processName(pid)) + " brought to the front");
    }
}

void GameHandOff::showGame(HWND game) {
    HWND away = nullptr;
    struct Ctx { GameHandOff* self; HWND* away; } ctx{ this, &away };
    {
        DpiScope dpi;
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || IsIconic(h)) return TRUE;
            RECT r; if (!GetWindowRect(h, &r)) return TRUE;
            MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
            if (!(mi.dwFlags & MONITORINFOF_PRIMARY)) { *c->away = h; return TRUE; }   // the touch screen's window stays
            ShowWindow(h, SW_MINIMIZE);
            c->self->minimisedFrontend_.push_back(h);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    }
    Sleep(300);
    // Supermodel draws nothing until its window loses the focus and gets it back: the touch window takes it for a moment
    if (away) { altTap(); SetForegroundWindow(away); Sleep(400); }
    altTap();
    ShowWindow(game, SW_RESTORE); BringWindowToTop(game); SetForegroundWindow(game);
    DWORD pid = 0; GetWindowThreadProcessId(game, &pid);
    LOG_INFO("GameHandOff", "game window " + narrow(processName(pid)) + " shown: frontend minimised, game activated" +
        std::string(away ? " (focus round trip via the touch screen)" : ""));
}

void GameHandOff::restoreFrontend() {
    for (HWND h : minimisedFrontend_) if (IsWindow(h)) ShowWindow(h, SW_RESTORE);
    minimisedFrontend_.clear();
}
#endif
