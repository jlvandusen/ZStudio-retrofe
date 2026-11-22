/* This file is part of RetroFE (ArcadeMaster extension). GPLv3 or later. */
#pragma once
#include "InputHandler.h"
#include "PointerInput.h"

// Binds one PointerInput gesture (touchTap, touch1SwipeUp, mouseWheelDown, mouseAxisX+, ...) to a RetroFE action.
// The shared recogniser does all the work; this just reports its state for the current frame.
class GestureHandler : public InputHandler {
public:
    GestureHandler(PointerInput& pointer, PointerInput::Gesture gesture, int screen = -1)
        : pointer_(pointer), gesture_(gesture), screen_(screen) {}
    bool update(SDL_Event& /*e*/) override { return false; }   // events are fed to PointerInput by UserInput
    bool pressed() override { return pointer_.active(gesture_, screen_); }
    void reset() override {}
    void updateKeystate() override {}
private:
    PointerInput& pointer_;
    PointerInput::Gesture gesture_;
    int screen_;
};

// A tap inside one cell of a column/row grid on a screen: on-screen touch buttons
// (e.g. "touch1Column3of5Row3of3" = middle button of a 5-button bar along the bottom of screen 1).
class RegionTapHandler : public InputHandler {
public:
    RegionTapHandler(PointerInput& pointer, int screen, int col, int cols, int row, int rows)
        : pointer_(pointer), screen_(screen), col_(col), cols_(cols), row_(row), rows_(rows) {}
    bool update(SDL_Event& /*e*/) override { return false; }
    bool pressed() override { return pointer_.tapInRegion(screen_, col_, cols_, row_, rows_); }
    void reset() override {}
    void updateKeystate() override {}
private:
    PointerInput& pointer_;
    int screen_, col_, cols_, row_, rows_;
};
