/* This file is part of RetroFE (ArcadeMaster extension: touch / trackball / spinner / wheel input).
 *
 * RetroFE is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */
#pragma once
#include <SDL.h>
#include <array>
#include <map>
#include <string>

class Configuration;

// One shared recogniser for pointer-style input. UserInput feeds it every SDL event and ticks it
// once per frame; GestureHandler / RegionTapHandler instances (one per controls.conf token) just query it.
//
//   touch:  tap, double tap, long press, two-finger tap, swipe (fling) x4, drag (held scroll) x4,
//           zone taps (left/right/top/bottom/center thirds), and REGION taps (column/row grid cells)
//           - per screen: "touch1Tap" only reacts to touches on RetroFE screen 1 (e.g. a pedestal /
//             marquee touchscreen), "touchTap" to any screen. Screens are RetroFE monitor numbers.
//   mouse:  wheel x4, relative axis x4 (trackballs, spinners: nudge = one step, spin = held scroll)
//   mouse-type touch panels: many cheap USB touch boards (e.g. ByQDtech 0483:5750 on HDMI pedestal panels) report
//           as an absolute MOUSE, not a digitizer, so Windows sends clicks, not fingers. controls.mouseTouchScreens
//           = "1" (comma list of RetroFE screens) turns a left press / drag / release inside those screens' windows
//           into a finger, so every touch gesture and region tap works; the cursor jump onto that screen is not
//           mistaken for trackball motion.
//
// Discrete gestures become a short "key tap" pulse (pulseMs) so RetroFE's state machine can't miss them;
// continuous ones (drag, axis) behave like a held key, so RetroFE's own scroll acceleration applies.
class PointerInput {
public:
    enum Gesture {
        Tap, DoubleTap, LongPress, TwoFingerTap,
        SwipeUp, SwipeDown, SwipeLeft, SwipeRight,
        DragUp, DragDown, DragLeft, DragRight,
        TapLeft, TapRight, TapTop, TapBottom, TapCenter,
        WheelUp, WheelDown, WheelLeft, WheelRight,
        AxisXMinus, AxisXPlus, AxisYMinus, AxisYPlus,
        GestureCount
    };
    static const int MaxScreens = 8;
    static const int Slots = MaxScreens + 1;          // slot 0 = any screen, slot s+1 = screen s

    explicit PointerInput(Configuration& config);
    void loadConfig();                 // controls.touch* / controls.mouseAxis* tunables
    void feed(const SDL_Event& e);     // every SDL event
    void tick();                       // once per frame, before handlers are polled
    bool active(Gesture g, int screen = -1) const { return active_[slotOf(screen)][g]; }
    // true while the last tap on `screen` (-1 = any) fell in grid cell (col of cols, row of rows), 1-based
    bool tapInRegion(int screen, int col, int cols, int row, int rows) const;
    void reset();
    // touch buttons (docs/22): the last tap, once - screen (-1 = unknown) and position in 0..1 of that screen's window.
    // When a layout button took it, suppressTap() stops the same tap also firing Tap / zone / region bindings.
    bool takeTap(int& screen, float& x, float& y);
    void suppressTap();

    // "touchSwipeUp" -> SwipeUp (screen -1), "touch1Tap" -> Tap on screen 1, "mouseAxisX+" -> AxisXPlus
    static bool parse(const std::string& lowered, Gesture& out, int& screen);
    // "touch1Column2of5", "touchRow3of3", "touch1Column1of5Row3of3" -> region tap
    static bool parseRegion(const std::string& lowered, int& screen, int& col, int& cols, int& row, int& rows);

private:
    static int slotOf(int screen) { return (screen < 0 || screen >= MaxScreens) ? 0 : screen + 1; }
    static int screenOfWindow(Uint32 windowID);
    void pulse(Gesture g, Uint32 now, int slot = 0);
    static const char* name(Gesture g);
    void hold(Gesture g, Uint32 now, Uint32 ms) { if (now + ms > pulseUntil_[0][g]) pulseUntil_[0][g] = now + ms; }

    struct Finger { float x0, y0, x, y; Uint32 t0; bool moved; int slot; };
    struct LastTap { Uint32 until = 0; float x = -1.f, y = -1.f; };

    Configuration& config_;
    std::array<std::array<bool, GestureCount>, Slots> active_{};
    std::array<std::array<Uint32, GestureCount>, Slots> pulseUntil_{};
    std::array<LastTap, Slots> lastRegionTap_{};
    struct PendingTap { bool valid = false; int slot = 0; float x = 0.f, y = 0.f; };
    PendingTap pendingTap_;

    std::map<SDL_FingerID, Finger> fingers_;
    int  maxFingersThisTouch_ = 0;
    bool longPressFired_ = false;
    std::array<Uint32, Slots> lastTapTime_{};
    std::array<float, Slots> lastTapX_{}, lastTapY_{};

    float axisAccX_ = 0.f, axisAccY_ = 0.f;

    // mouse-type touch panels (controls.mouseTouchScreens)
    bool isMouseTouchScreen(int screen) const { return screen >= 0 && screen < MaxScreens && mouseTouchScreen_[screen]; }
    void feedMouseFinger(Uint32 type, Uint32 windowID, Sint32 x, Sint32 y);
    std::array<bool, MaxScreens> mouseTouchScreen_{};
    void parkCursor();
    bool anyMouseTouch() const { for (bool b : mouseTouchScreen_) if (b) return true; return false; }
    bool mouseTouchDown_ = false;
    bool warpPending_ = false; int warpX_ = 0, warpY_ = 0;
    Uint32 mouseTouchWindow_ = 0;
    float  mouseTouchJumpPx_ = 150.f;  // one motion event this large onto a touch screen = the touch's cursor jump

    // tunables (normalised screen units for touch, pixels for mouse axes)
    Uint32 pulseMs_ = 90;
    Uint32 tapMaxMs_ = 300;
    Uint32 doubleTapMs_ = 350;
    Uint32 longPressMs_ = 650;
    Uint32 swipeMaxMs_ = 450;
    float  moveTolerance_ = 0.03f;
    float  swipeMinDist_ = 0.12f;
    float  dragDeadzone_ = 0.05f;
    float  axisStep_ = 30.f;
    Uint32 axisHoldMs_ = 120;
};
