#pragma once

#include "InputHandler.h"
#include <map>
#include <utility>

class JoyAxisHandler : public InputHandler
{
public:
    JoyAxisHandler(SDL_JoystickID joyid, Uint8 axis, Sint16 min, Sint16 max);
    bool update(SDL_Event &e);
    bool pressed();
    void reset();
    void updateKeystate();

    // ArcadeMaster: pedal-style axes (wheel pedals, handbrakes) rest at an EXTREME, not at centre. A released
    // pedal therefore looked "held" to the binding on its resting side (e.g. "down = joyAxis1+"), which froze
    // menus in a permanent scroll. Pedal axes learn their resting extreme; a binding toward rest never fires.
    static void registerDevice(int slot, SDL_Joystick* joy);
    static void clearDevice(int slot);

private:
    // guessed: pedal only because it started at an extreme (not a wheel axis) - undone if it is later seen resting at centre
    struct AxisState { bool pedal = false; bool guessed = false; bool restKnown = false; Sint16 rest = 0; Sint16 last = 0; Uint32 lastChange = 0; bool seen = false; };
    static std::map<std::pair<int, int>, AxisState> axes_;
    static bool isExtreme(Sint16 v) { return v >= 30000 || v <= -30000; }
    bool restSideBlocked(const AxisState& a) const { return a.pedal && a.restKnown && min_ <= a.rest && a.rest <= max_; }

    SDL_JoystickID joyid_;
    Uint8 axis_;
    Sint16 min_;
    Sint16 max_;
    int lastWhich_ = -1;

    bool pressed_{ false };
};
