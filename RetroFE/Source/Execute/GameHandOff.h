/* This file is part of RetroFE (ZStudio ArcadeMaster fork).
 *
 * RetroFE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
// The game hand-off (docs/21 phase 2): while a launched game starts, the frontend gets it on screen and in front -
// what ArcadeMaster's AmPanel did from outside (LoadingScreen.KeepGameOnMainScreen / BringForward / ShowGame).
// Off unless settings.conf has "gameHandOff = true"; AmPanel steps aside when it is on.
#pragma once
#include <string>

#ifdef WIN32
#include <Windows.h>
#include <chrono>
#include <map>
#include <set>
#include <vector>

class GameHandOff {
public:
    // takes the baseline (the big windows up before the launch): anything new after that can be the game
    explicit GameHandOff(bool enabled);
    ~GameHandOff();                     // the frontend windows minimised for the game come back
    void tick();                        // from the launcher's wait loop; does work at most every 400 ms
    bool enabled() const { return enabled_; }

private:
    struct Win { HWND hwnd; DWORD pid; std::wstring process; RECT rect; RECT monitor; bool primary; };
    std::vector<Win> bigWindows(double cover);
    void minimiseEmulatorConsoles();
    void bringForward(HWND game);
    void showGame(HWND game);
    void restoreFrontend();

    bool enabled_ = false;
    std::chrono::steady_clock::time_point start_, lastTick_;
    std::set<HWND> baseline_, moved_, shown_, broughtUp_, consolesDone_;
    std::map<HWND, std::chrono::steady_clock::time_point> seen_;
    std::vector<HWND> minimisedFrontend_;
};
#else
class GameHandOff {
public:
    explicit GameHandOff(bool) {}
    void tick() {}
    bool enabled() const { return false; }
};
#endif
