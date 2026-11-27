/* This file is part of RetroFE (ArcadeMaster extension: touch / trackball / spinner / wheel input).
 *
 * RetroFE is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 */
#include "PointerInput.h"
#include "../Database/Configuration.h"
#include "../Utility/Log.h"
#include "../SDL.h"
#include <cmath>
#include <regex>
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
    if (config_.getProperty("controls.mouseTouchJumpPx", f) && f > 0)    mouseTouchJumpPx_ = f;
    mouseTouchScreen_.fill(false);
    std::string screens;
    if (config_.getProperty("controls.mouseTouchScreens", screens)) {      // "1" or "1,2"
        for (char c : screens) if (c >= '0' && c < '0' + MaxScreens) mouseTouchScreen_[c - '0'] = true;
    }
}

// hidden cursor back to the middle of screen 0; the motion event the warp generates is skipped in feed()
void PointerInput::parkCursor() {
    SDL_Window* w0 = SDL::getWindow(0); int ww = 0, wh = 0; if (w0) SDL_GetWindowSize(w0, &ww, &wh);
    if (!w0 || ww <= 0 || wh <= 0) return;
    warpX_ = ww / 2; warpY_ = wh / 2; warpPending_ = true;
    SDL_WarpMouseInWindow(w0, warpX_, warpY_);
}

// a left press / drag / release from a mouse-type touch panel becomes finger 0x7A7A on that window
void PointerInput::feedMouseFinger(Uint32 type, Uint32 windowID, Sint32 x, Sint32 y) {
    SDL_Window* w = SDL_GetWindowFromID(windowID);
    int ww = 0, wh = 0; if (w) SDL_GetWindowSize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    SDL_Event f{};
    f.type = type;
    f.tfinger.touchId = 0; f.tfinger.fingerId = 0x7A7A;
    f.tfinger.x = std::fmin(1.f, std::fmax(0.f, static_cast<float>(x) / ww));
    f.tfinger.y = std::fmin(1.f, std::fmax(0.f, static_cast<float>(y) / wh));
    f.tfinger.pressure = 1.f; f.tfinger.windowID = windowID;
    feed(f);
}

const char* PointerInput::name(Gesture g) {
    static const char* n[GestureCount] = { "Tap", "DoubleTap", "LongPress", "TwoFingerTap", "SwipeUp", "SwipeDown", "SwipeLeft", "SwipeRight",
        "DragUp", "DragDown", "DragLeft", "DragRight", "TapLeft", "TapRight", "TapTop", "TapBottom", "TapCenter",
        "WheelUp", "WheelDown", "WheelLeft", "WheelRight", "AxisX-", "AxisX+", "AxisY-", "AxisY+" };
    return (g >= 0 && g < GestureCount) ? n[g] : "?";
}

// RetroFE monitor number of the window under the finger (-1 if unknown, e.g. a touch outside our windows)
int PointerInput::screenOfWindow(Uint32 windowID) {
    if (windowID == 0) {
        // ArcadeMaster: on Windows a touch screen's finger events can come without a window (windowID 0) - the pedestal's
        // touch bar never answered ("screen -1"). Windows still moves the pointer to the touch, so the screen whose
        // window holds the pointer is the one touched.
        int mx = 0, my = 0;
        SDL_GetGlobalMouseState(&mx, &my);
        for (int s = 0; s < SDL::getScreenCount() && s < MaxScreens; ++s) {
            SDL_Window* w = SDL::getWindow(s);
            if (!w) continue;
            int wx = 0, wy = 0, ww = 0, wh = 0;
            SDL_GetWindowPosition(w, &wx, &wy);
            SDL_GetWindowSize(w, &ww, &wh);
            if (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh) return s;
        }
        // ArcadeMaster: the pointer check missed (the pedestal kept "screen -1"): a cabinet's touch panel is its extra
        // screen, so with more than one screen the last one is the touched one
        const int n = SDL::getScreenCount() < MaxScreens ? SDL::getScreenCount() : MaxScreens;
        LOG_DEBUG("Pointer", "touch without a window, pointer " << mx << "," << my << " on no screen - using screen " << (n > 1 ? n - 1 : -1));
        return n > 1 ? n - 1 : -1;
    }
    for (int s = 0; s < SDL::getScreenCount() && s < MaxScreens; ++s) {
        SDL_Window* w = SDL::getWindow(s);
        if (w && SDL_GetWindowID(w) == windowID) return s;
    }
    // ArcadeMaster: a window ID that is none of ours (the pedestal's touch screen reports one) - treat it like no window
    LOG_DEBUG("Pointer", "touch on window " << windowID << ", not one of ours");
    return screenOfWindow(0);
}

void PointerInput::pulse(Gesture g, Uint32 now, int slot) {
    pulseUntil_[0][g] = now + pulseMs_;                                    // "any screen"
    if (slot > 0) pulseUntil_[slot][g] = now + pulseMs_;                   // and the screen it happened on
    LOG_DEBUG("Pointer", std::string("gesture ") + name(g) + (slot > 0 ? " on screen " + std::to_string(slot - 1) : ""));
}

void PointerInput::reset() {
    for (auto& a : active_) a.fill(false);
    for (auto& p : pulseUntil_) p.fill(0);
    lastRegionTap_.fill(LastTap{});
    pendingTap_ = PendingTap{};
    fingers_.clear();
    maxFingersThisTouch_ = 0;
    longPressFired_ = false;
    axisAccX_ = axisAccY_ = 0.f;
}

bool PointerInput::takeTap(int& screen, float& x, float& y) {
    if (!pendingTap_.valid) return false;
    screen = pendingTap_.slot - 1; x = pendingTap_.x; y = pendingTap_.y;
    pendingTap_.valid = false;
    return true;
}

void PointerInput::suppressTap() {
    // the tap was a layout button's: no Tap / zone / double-tap pulse and no region tap for it, on any screen
    for (auto& p : pulseUntil_) for (Gesture g : { Tap, DoubleTap, TapLeft, TapRight, TapTop, TapBottom, TapCenter }) p[g] = 0;
    lastRegionTap_.fill(LastTap{});
}

bool PointerInput::tapInRegion(int screen, int col, int cols, int row, int rows) const {
    const LastTap& t = lastRegionTap_[slotOf(screen)];
    if (SDL_GetTicks() >= t.until) return false;
    if (cols > 0) { int c = static_cast<int>(t.x * cols) + 1; if (c > cols) c = cols; if (c != col) return false; }
    if (rows > 0) { int r = static_cast<int>(t.y * rows) + 1; if (r > rows) r = rows; if (r != row) return false; }
    return true;
}

void PointerInput::feed(const SDL_Event& e) {
    const Uint32 now = SDL_GetTicks();
    if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERUP)
        LOG_DEBUG("Pointer", (e.type == SDL_FINGERDOWN ? "finger down " : "finger up ") << e.tfinger.fingerId << " @ " << e.tfinger.x << "," << e.tfinger.y
                  << " screen " << screenOfWindow(e.tfinger.windowID));
    else if (e.type == SDL_MOUSEWHEEL)
        LOG_DEBUG("Pointer", "wheel " << e.wheel.x << "," << e.wheel.y << " which=" << e.wheel.which);
    switch (e.type) {
    case SDL_FINGERDOWN: {
        if (fingers_.empty()) { maxFingersThisTouch_ = 0; longPressFired_ = false; }
        const int slot = slotOf(screenOfWindow(e.tfinger.windowID));
        fingers_[e.tfinger.fingerId] = Finger{ e.tfinger.x, e.tfinger.y, e.tfinger.x, e.tfinger.y, now, false, slot };
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
        const int s = f.slot;

        if (maxFingersThisTouch_ >= 2) {
            // two-finger tap: fires once when the last finger of a short, still, multi-finger touch lifts
            if (fingers_.empty() && dt <= tapMaxMs_ + 150 && !f.moved) pulse(TwoFingerTap, now, s);
            break;
        }
        if (longPressFired_) break;                                        // the press already did its job

        if (!f.moved && dt <= tapMaxMs_) {
            pulse(Tap, now, s);
            for (int slot : { 0, s }) { lastRegionTap_[slot].until = now + pulseMs_; lastRegionTap_[slot].x = f.x0; lastRegionTap_[slot].y = f.y0; }
            pendingTap_ = PendingTap{ true, s, f.x0, f.y0 };               // for layout touch buttons (takeTap)
            if (f.x0 < 1.f / 3) pulse(TapLeft, now, s); else if (f.x0 > 2.f / 3) pulse(TapRight, now, s);
            if (f.y0 < 1.f / 3) pulse(TapTop, now, s);  else if (f.y0 > 2.f / 3) pulse(TapBottom, now, s);
            if (f.x0 >= 1.f / 3 && f.x0 <= 2.f / 3 && f.y0 >= 1.f / 3 && f.y0 <= 2.f / 3) pulse(TapCenter, now, s);
            if (now - lastTapTime_[s] <= doubleTapMs_ &&
                std::fabs(f.x0 - lastTapX_[s]) < moveTolerance_ * 2 && std::fabs(f.y0 - lastTapY_[s]) < moveTolerance_ * 2) {
                pulse(DoubleTap, now, s);
                lastTapTime_[s] = 0;                                       // a triple tap is not two doubles
            } else {
                lastTapTime_[s] = now; lastTapX_[s] = f.x0; lastTapY_[s] = f.y0;
            }
        }
        else if (dt <= swipeMaxMs_ && (std::fabs(dx) >= swipeMinDist_ || std::fabs(dy) >= swipeMinDist_)) {
            if (std::fabs(dy) >= std::fabs(dx)) pulse(dy < 0 ? SwipeUp : SwipeDown, now, s);
            else                                pulse(dx < 0 ? SwipeLeft : SwipeRight, now, s);
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
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (e.button.which == SDL_TOUCH_MOUSEID || e.button.button != SDL_BUTTON_LEFT) break;
        if (e.type == SDL_MOUSEBUTTONDOWN) {
            if (!isMouseTouchScreen(screenOfWindow(e.button.windowID))) break;
            mouseTouchDown_ = true; mouseTouchWindow_ = e.button.windowID;
            axisAccX_ = axisAccY_ = 0.f;                                   // the press is not trackball motion
            feedMouseFinger(SDL_FINGERDOWN, e.button.windowID, e.button.x, e.button.y);
        } else if (mouseTouchDown_) {
            mouseTouchDown_ = false; axisAccX_ = axisAccY_ = 0.f;
            feedMouseFinger(SDL_FINGERUP, mouseTouchWindow_, e.button.x, e.button.y);
            parkCursor();                                                  // the trackball carries on from screen 0

        }
        break;
    }
    case SDL_MOUSEMOTION: {
        if (e.motion.which == SDL_TOUCH_MOUSEID) break;                    // touch already handled as fingers
        if (mouseTouchDown_) { feedMouseFinger(SDL_FINGERMOTION, mouseTouchWindow_, e.motion.x, e.motion.y); break; }
        // the cursor teleporting onto a mouse-type touch screen (a new touch) is not a trackball spin
        if (isMouseTouchScreen(screenOfWindow(e.motion.windowID)) &&
            std::fabs(static_cast<float>(e.motion.xrel)) + std::fabs(static_cast<float>(e.motion.yrel)) >= mouseTouchJumpPx_) { axisAccX_ = axisAccY_ = 0.f; break; }
        if (warpPending_ && e.motion.x == warpX_ && e.motion.y == warpY_) { warpPending_ = false; break; }   // our own re-centre
        axisAccX_ += static_cast<float>(e.motion.xrel);
        axisAccY_ += static_cast<float>(e.motion.yrel);
        // without relative mode (mouse-type touch panel present) the trackball moves a hidden desktop cursor: keep it in
        // the middle of screen 0 so it never stalls at a desktop edge or wanders onto the touch screen
        if (anyMouseTouch()) {
            SDL_Window* w0 = SDL::getWindow(0); int ww = 0, wh = 0; if (w0) SDL_GetWindowSize(w0, &ww, &wh);
            const bool onMain = w0 && e.motion.windowID == SDL_GetWindowID(w0);
            const int m = 16;
            if (!isMouseTouchScreen(screenOfWindow(e.motion.windowID)) &&      // never pull the cursor off the touch panel
                w0 && ww > 2 * m && wh > 2 * m && (!onMain || e.motion.x < m || e.motion.y < m || e.motion.x > ww - m || e.motion.y > wh - m))
                parkCursor();
        }
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
    // a mouse-type touch panel needs absolute clicks on its own screen: RetroFE turns relative mouse mode on at SDL init
    // (and again after every game), before controls.conf is read - so switch it off here, every frame it comes back
    if (anyMouseTouch() && SDL_GetRelativeMouseMode()) { SDL_SetRelativeMouseMode(SDL_FALSE); parkCursor(); }
    std::array<std::array<bool, 4>, Slots> drag{};                         // U D L R per slot

    if (fingers_.size() == 1 && maxFingersThisTouch_ == 1 && !longPressFired_) {
        const Finger& f = fingers_.begin()->second;
        // long press: one still finger held past the threshold
        if (!f.moved && now - f.t0 >= longPressMs_) { pulse(LongPress, now, f.slot); longPressFired_ = true; }
        // drag: one finger held away from where it started -> behave like a held direction key
        else {
            const float dx = f.x - f.x0, dy = f.y - f.y0; int d = -1;
            if (std::fabs(dy) >= std::fabs(dx)) { if (dy <= -dragDeadzone_) d = 0; else if (dy >= dragDeadzone_) d = 1; }
            else                                { if (dx <= -dragDeadzone_) d = 2; else if (dx >= dragDeadzone_) d = 3; }
            if (d >= 0) { drag[0][d] = true; if (f.slot > 0) drag[f.slot][d] = true; }
        }
    }

    // slowly bleed off sub-step trackball motion so tiny drifts never add up to a phantom step
    axisAccX_ *= 0.9f; axisAccY_ *= 0.9f;

    for (int s = 0; s < Slots; ++s) {
        for (int g = 0; g < GestureCount; ++g) active_[s][g] = now < pulseUntil_[s][g];
        active_[s][DragUp] = drag[s][0]; active_[s][DragDown] = drag[s][1]; active_[s][DragLeft] = drag[s][2]; active_[s][DragRight] = drag[s][3];
    }
}

// "touch1tapleft" -> (TapLeft, screen 1); "touchtap" -> (Tap, -1); mouse tokens are never screen-specific
bool PointerInput::parse(const std::string& s, Gesture& out, int& screen) {
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
    std::string key = s; screen = -1;
    if (key.size() > 5 && key.compare(0, 5, "touch") == 0 && std::isdigit(static_cast<unsigned char>(key[5]))) {
        screen = key[5] - '0'; key.erase(5, 1);                            // "touch1tap" -> "touchtap" on screen 1
    }
    for (const auto& n : names) { if (key == n.first) { out = n.second; return true; } }
    return false;
}

bool PointerInput::parseRegion(const std::string& s, int& screen, int& col, int& cols, int& row, int& rows) {
    static const std::regex re("^touch(\\d)?(?:column(\\d+)of(\\d+))?(?:row(\\d+)of(\\d+))?$");
    std::smatch m;
    if (!std::regex_match(s, m, re) || (!m[2].matched && !m[4].matched)) return false;
    screen = m[1].matched ? std::stoi(m[1].str()) : -1;
    col = m[2].matched ? std::stoi(m[2].str()) : 0; cols = m[3].matched ? std::stoi(m[3].str()) : 0;
    row = m[4].matched ? std::stoi(m[4].str()) : 0; rows = m[5].matched ? std::stoi(m[5].str()) : 0;
    return (cols == 0 || (col >= 1 && col <= cols)) && (rows == 0 || (row >= 1 && row <= rows));
}
