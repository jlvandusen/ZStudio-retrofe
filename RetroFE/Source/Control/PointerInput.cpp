/* This file is part of RetroFE (ArcadeMaster extension: touch / trackball / spinner / wheel input).
 *
 * RetroFE is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */
#include "PointerInput.h"
#include "../Database/Configuration.h"
#include "../Utility/Log.h"
#include <cmath>
#include <utility>

PointerInput::PointerInput(Configuration& config) : config_(config) { loadConfig(); }

void PointerInput::loadConfig() {
    int i = 0; float f = 0.f;
    if (config_.getProperty("controls.touchPulseMs", i) && i > 0)        pulseMs_ = static_cast<Uint32>(i);
    if (config_.getProperty("controls.touchTapMaxMs", i) && i > 0)       tapMaxMs_ = static_cast<Uint32>(i);
    if (config_.getProperty("controls.touchDoubleTapMs", i) && i > 0)    doubleTapMs_ = static_cast<Uint32>(i);
    if (config_.getProperty("controls.touchLongPressMs", i) && i > 0)    longPressMs_ = static_cast<Uint32>(i);
    if (config_.getProperty("controls.touchSwipeMaxMs", i) && i > 0)     swipeMaxMs_ = static_cast<Uint32>(i);
    if (config_.getProperty("controls.touchMoveTolerance", f) && f > 0)  moveTolerance_ = f;   // fraction of screen
    if (config_.getProperty("controls.touchSwipeMinDistance", f) && f > 0) swipeMinDist_ = f;  // fraction of screen
    if (config_.getProperty("controls.touchDragDeadzone", f) && f > 0)   dragDeadzone_ = f;    // fraction of screen
    if (config_.getProperty("controls.mouseAxisStep", f) && f > 0)       axisStep_ = f;        // pixels (counts) per step
    if (config_.getProperty("controls.mouseAxisHoldMs", i) && i > 0)     axisHoldMs_ = static_cast<Uint32>(i);
}

const char* PointerInput::name(Gesture g) {
    static const char* n[GestureCount] = { "Tap", "DoubleTap", "LongPress", "TwoFingerTap", "SwipeUp", "SwipeDown", "SwipeLeft", "SwipeRight",
        "DragUp", "DragDown", "DragLeft", "DragRight", "TapLeft", "TapRight", "TapTop", "TapBottom", "TapCenter",
        "WheelUp", "WheelDown", "WheelLeft", "WheelRight", "AxisX-", "AxisX+", "AxisY-", "AxisY+" };
    return (g >= 0 && g < GestureCount) ? n[g] : "?";
}

void PointerInput::pulse(Gesture g, Uint32 now) {
    pulseUntil_[g] = now + pulseMs_;
    LOG_DEBUG("Pointer", std::string("gesture ") + name(g));
}

void PointerInput::reset() {
    active_.fill(false);
    pulseUntil_.fill(0);
    fingers_.clear();
    maxFingersThisTouch_ = 0;
    longPressFired_ = false;
    axisAccX_ = axisAccY_ = 0.f;
}

void PointerInput::feed(const SDL_Event& e) {
    const Uint32 now = SDL_GetTicks();
    if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERUP)
        LOG_DEBUG("Pointer", (e.type == SDL_FINGERDOWN ? "finger down " : "finger up ") << e.tfinger.fingerId << " @ " << e.tfinger.x << "," << e.tfinger.y);
    else if (e.type == SDL_MOUSEWHEEL)
        LOG_DEBUG("Pointer", "wheel " << e.wheel.x << "," << e.wheel.y << " which=" << e.wheel.which);
    switch (e.type) {
    case SDL_FINGERDOWN: {
        if (fingers_.empty()) { maxFingersThisTouch_ = 0; longPressFired_ = false; }
        fingers_[e.tfinger.fingerId] = Finger{ e.tfinger.x, e.tfinger.y, e.tfinger.x, e.tfinger.y, now, false };
        if (static_cast<int>(fingers_.size()) > maxFingersThisTouch_) maxFingersThisTouch_ = static_cast<int>(fingers_.size());
        break;
    }
    case SDL_FINGERMOTION: {
        auto it = fingers_.find(e.tfinger.fingerId);
        if (it == fingers_.end()) break;
        Finger& f = it->second;
        f.x = e.tfinger.x; f.y = e.tfinger.y;
        if (std::fabs(f.x - f.x0) > moveTolerance_ || std::fabs(f.y - f.y0) > moveTolerance_) f.moved = true;
        break;
    }
    case SDL_FINGERUP: {
        auto it = fingers_.find(e.tfinger.fingerId);
        if (it == fingers_.end()) break;
        const Finger f = it->second;
        fingers_.erase(it);
        const Uint32 dt = now - f.t0;
        const float dx = e.tfinger.x - f.x0, dy = e.tfinger.y - f.y0;

        if (maxFingersThisTouch_ >= 2) {
            // two-finger tap: fires once when the last finger of a short, still, multi-finger touch lifts
            if (fingers_.empty() && dt <= tapMaxMs_ + 150 && !f.moved) pulse(TwoFingerTap, now);
            break;
        }
        if (longPressFired_) break;                                        // the press already did its job

        if (!f.moved && dt <= tapMaxMs_) {
            pulse(Tap, now);
            if (f.x0 < 1.f / 3) pulse(TapLeft, now); else if (f.x0 > 2.f / 3) pulse(TapRight, now);
            if (f.y0 < 1.f / 3) pulse(TapTop, now);  else if (f.y0 > 2.f / 3) pulse(TapBottom, now);
            if (f.x0 >= 1.f / 3 && f.x0 <= 2.f / 3 && f.y0 >= 1.f / 3 && f.y0 <= 2.f / 3) pulse(TapCenter, now);
            if (now - lastTapTime_ <= doubleTapMs_ &&
                std::fabs(f.x0 - lastTapX_) < moveTolerance_ * 2 && std::fabs(f.y0 - lastTapY_) < moveTolerance_ * 2) {
                pulse(DoubleTap, now);
                lastTapTime_ = 0;                                          // a triple tap is not two doubles
            } else {
                lastTapTime_ = now; lastTapX_ = f.x0; lastTapY_ = f.y0;
            }
        }
        else if (dt <= swipeMaxMs_ && (std::fabs(dx) >= swipeMinDist_ || std::fabs(dy) >= swipeMinDist_)) {
            if (std::fabs(dy) >= std::fabs(dx)) pulse(dy < 0 ? SwipeUp : SwipeDown, now);
            else                                pulse(dx < 0 ? SwipeLeft : SwipeRight, now);
        }
        break;
    }
    case SDL_MOUSEWHEEL: {
        if (e.wheel.which == SDL_TOUCH_MOUSEID) break;
        int y = e.wheel.y, x = e.wheel.x;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { y = -y; x = -x; }
        if (y > 0) pulse(WheelUp, now);   else if (y < 0) pulse(WheelDown, now);
        if (x > 0) pulse(WheelRight, now); else if (x < 0) pulse(WheelLeft, now);
        break;
    }
    case SDL_MOUSEMOTION: {
        if (e.motion.which == SDL_TOUCH_MOUSEID) break;                    // touch already handled as fingers
        axisAccX_ += static_cast<float>(e.motion.xrel);
        axisAccY_ += static_cast<float>(e.motion.yrel);
        // each full step re-arms a short hold: a nudge = one tap, a spin = held key (RetroFE accelerates)
        while (axisAccX_ >= axisStep_)  { hold(AxisXPlus, now, axisHoldMs_);  axisAccX_ -= axisStep_; }
        while (axisAccX_ <= -axisStep_) { hold(AxisXMinus, now, axisHoldMs_); axisAccX_ += axisStep_; }
        while (axisAccY_ >= axisStep_)  { hold(AxisYPlus, now, axisHoldMs_);  axisAccY_ -= axisStep_; }
        while (axisAccY_ <= -axisStep_) { hold(AxisYMinus, now, axisHoldMs_); axisAccY_ += axisStep_; }
        break;
    }
    default: break;
    }
}

void PointerInput::tick() {
    const Uint32 now = SDL_GetTicks();

    // long press: one still finger held past the threshold
    if (fingers_.size() == 1 && maxFingersThisTouch_ == 1 && !longPressFired_) {
        const Finger& f = fingers_.begin()->second;
        if (!f.moved && now - f.t0 >= longPressMs_) { pulse(LongPress, now); longPressFired_ = true; }
    }

    // drag: one finger held away from where it started -> behave like a held direction key
    bool dragU = false, dragD = false, dragL = false, dragR = false;
    if (fingers_.size() == 1 && maxFingersThisTouch_ == 1 && !longPressFired_) {
        const Finger& f = fingers_.begin()->second;
        const float dx = f.x - f.x0, dy = f.y - f.y0;
        if (std::fabs(dy) >= std::fabs(dx)) { if (dy <= -dragDeadzone_) dragU = true; else if (dy >= dragDeadzone_) dragD = true; }
        else                                { if (dx <= -dragDeadzone_) dragL = true; else if (dx >= dragDeadzone_) dragR = true; }
    }

    // slowly bleed off sub-step trackball motion so tiny drifts never add up to a phantom step
    axisAccX_ *= 0.9f; axisAccY_ *= 0.9f;

    for (int g = 0; g < GestureCount; ++g) active_[g] = now < pulseUntil_[g];
    active_[DragUp] = dragU; active_[DragDown] = dragD; active_[DragLeft] = dragL; active_[DragRight] = dragR;
}

bool PointerInput::parse(const std::string& s, Gesture& out) {
    static const std::pair<const char*, Gesture> names[] = {
        {"touchtap", Tap}, {"touchdoubletap", DoubleTap}, {"touchlongpress", LongPress}, {"touchtwofingertap", TwoFingerTap},
        {"touchswipeup", SwipeUp}, {"touchswipedown", SwipeDown}, {"touchswipeleft", SwipeLeft}, {"touchswiperight", SwipeRight},
        {"touchdragup", DragUp}, {"touchdragdown", DragDown}, {"touchdragleft", DragLeft}, {"touchdragright", DragRight},
        {"touchtapleft", TapLeft}, {"touchtapright", TapRight}, {"touchtaptop", TapTop}, {"touchtapbottom", TapBottom}, {"touchtapcenter", TapCenter},
        {"mousewheelup", WheelUp}, {"mousewheeldown", WheelDown}, {"mousewheelleft", WheelLeft}, {"mousewheelright", WheelRight},
        {"mouseaxisx-", AxisXMinus}, {"mouseaxisx+", AxisXPlus}, {"mouseaxisy-", AxisYMinus}, {"mouseaxisy+", AxisYPlus},
        {"mousex-", AxisXMinus}, {"mousex+", AxisXPlus}, {"mousey-", AxisYMinus}, {"mousey+", AxisYPlus},
        {"spinnerleft", AxisXMinus}, {"spinnerright", AxisXPlus},
        {"trackballleft", AxisXMinus}, {"trackballright", AxisXPlus}, {"trackballup", AxisYMinus}, {"trackballdown", AxisYPlus},
    };
    for (const auto& n : names) { if (s == n.first) { out = n.second; return true; } }
    return false;
}
