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
// once per frame; GestureHandler instances (one per controls.conf token) just query it.
//
//   touch:  tap, double tap, long press, two-finger tap, swipe (fling) x4, drag (held scroll) x4,
//           zone taps (left/right/top/bottom/center thirds of the screen)
//   mouse:  wheel x4, relative axis x4 (trackballs, spinners: nudge = one step, spin = held scroll)
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

    explicit PointerInput(Configuration& config);
    void loadConfig();                 // controls.touch* / controls.mouseAxis* tunables
    void feed(const SDL_Event& e);     // every SDL event
    void tick();                       // once per frame, before handlers are polled
    bool active(Gesture g) const { return active_[g]; }
    void reset();

    // "touchSwipeUp" -> SwipeUp, "mouseAxisX+" -> AxisXPlus ... returns false if unknown
    static bool parse(const std::string& lowered, Gesture& out);

private:
    void pulse(Gesture g, Uint32 now);
    static const char* name(Gesture g);
    void hold(Gesture g, Uint32 now, Uint32 ms) { if (now + ms > pulseUntil_[g]) pulseUntil_[g] = now + ms; }

    struct Finger { float x0, y0, x, y; Uint32 t0; bool moved; };

    Configuration& config_;
    std::array<bool, GestureCount> active_{};
    std::array<Uint32, GestureCount> pulseUntil_{};

    std::map<SDL_FingerID, Finger> fingers_;
    int  maxFingersThisTouch_ = 0;
    bool longPressFired_ = false;
    Uint32 lastTapTime_ = 0;
    float lastTapX_ = -1.f, lastTapY_ = -1.f;

    float axisAccX_ = 0.f, axisAccY_ = 0.f;

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
