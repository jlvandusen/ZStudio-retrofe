/* This file is part of RetroFE (ArcadeMaster extension). GPLv3 or later. */
#pragma once
#include "InputHandler.h"
#include "PointerInput.h"

// Binds one PointerInput gesture (touchTap, touchSwipeUp, mouseWheelDown, mouseAxisX+, ...) to a RetroFE action.
// The shared recogniser does all the work; this just reports its state for the current frame.
class GestureHandler : public InputHandler {
public:
    GestureHandler(PointerInput& pointer, PointerInput::Gesture gesture) : pointer_(pointer), gesture_(gesture) {}
    bool update(SDL_Event& /*e*/) override { return false; }   // events are fed to PointerInput by UserInput
    bool pressed() override { return pointer_.active(gesture_); }
    void reset() override {}
    void updateKeystate() override {}
private:
    PointerInput& pointer_;
    PointerInput::Gesture gesture_;
};
