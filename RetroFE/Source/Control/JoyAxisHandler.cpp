#include "JoyAxisHandler.h"
#include "../Utility/Log.h"
#include <cctype>
#include <string>

std::map<std::pair<int, int>, JoyAxisHandler::AxisState> JoyAxisHandler::axes_;

JoyAxisHandler::JoyAxisHandler(SDL_JoystickID joyid, Uint8 axis, Sint16 min, Sint16 max)
: joyid_(joyid)
, axis_(axis)
, min_(min)
, max_(max)
{
}

// Called by UserInput when a joystick is assigned to a slot (slot = the remapped e.jaxis.which).
void JoyAxisHandler::registerDevice(int slot, SDL_Joystick* joy)
{
    if (!joy) return;
    std::string name = SDL_JoystickName(joy) ? SDL_JoystickName(joy) : "";
    for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool wheel = SDL_JoystickGetType(joy) == SDL_JOYSTICK_TYPE_WHEEL || name.find("wheel") != std::string::npos || name.find("racing") != std::string::npos;
    const int n = SDL_JoystickNumAxes(joy);
    for (int a = 0; a < n; ++a) {
        AxisState s;
        Sint16 init = 0;
        const bool hasInit = SDL_JoystickGetAxisInitialState(joy, a, &init) == SDL_TRUE;
        // wheel: every axis except steering (0) is a pedal; any device: an axis that starts at an extreme rests there
        s.pedal = (wheel && a != 0) || (hasInit && isExtreme(init));
        s.guessed = s.pedal && !(wheel && a != 0);
        if (hasInit && isExtreme(init)) { s.rest = init; s.restKnown = true; }
        s.last = init; s.lastChange = SDL_GetTicks();
        axes_[{ slot, a }] = s;
        if (s.pedal) LOG_INFO("Input", "joystick slot " + std::to_string(slot) + " (" + name + ") axis " + std::to_string(a) + " treated as a pedal" +
                              (s.restKnown ? " (rests at " + std::to_string(s.rest) + ")" : " (rest learned on first release)"));
    }
}

void JoyAxisHandler::clearDevice(int slot)
{
    for (auto it = axes_.begin(); it != axes_.end();) { if (it->first.first == slot) it = axes_.erase(it); else ++it; }
}

void JoyAxisHandler::reset()
{
    pressed_= false;
}

bool JoyAxisHandler::update(SDL_Event &e)
{
    if(e.type != SDL_JOYAXISMOTION || (joyid_ != -1 && e.jaxis.which != joyid_) || e.jaxis.axis != axis_) return false;
    lastWhich_ = e.jaxis.which;
    AxisState& a = axes_[{ static_cast<int>(e.jaxis.which), static_cast<int>(axis_) }];
    if (!a.seen || a.last != e.jaxis.value) { a.last = e.jaxis.value; a.lastChange = SDL_GetTicks(); a.seen = true; }
    pressed_ = (min_ <= e.jaxis.value && e.jaxis.value <= max_) && !restSideBlocked(a);

    return true;
}

// Per frame: pedals send no events while resting, so learn the rest position here and release the binding.
void JoyAxisHandler::updateKeystate()
{
    if (lastWhich_ < 0) return;
    auto it = axes_.find({ lastWhich_, static_cast<int>(axis_) });
    if (it == axes_.end()) return;
    AxisState& a = it->second;
    // a guessed pedal that settles near centre is really a stick (it was just held at boot): restore normal behaviour
    if (a.pedal && a.guessed && a.seen && a.last > -8000 && a.last < 8000 && SDL_GetTicks() - a.lastChange >= 250) {
        a.pedal = false; a.restKnown = false;
        LOG_INFO("Input", "slot " + std::to_string(lastWhich_) + " axis " + std::to_string(axis_) + " centres - treated as a stick, not a pedal");
    }
    if (a.pedal && isExtreme(a.last) && SDL_GetTicks() - a.lastChange >= 250 && (!a.restKnown || a.rest != a.last)) {
        a.rest = a.last; a.restKnown = true;
        LOG_INFO("Input", "pedal slot " + std::to_string(lastWhich_) + " axis " + std::to_string(axis_) + " rests at " + std::to_string(a.rest));
    }
    if (restSideBlocked(a) && pressed_ && min_ <= a.last && a.last <= max_) pressed_ = false;
}

bool JoyAxisHandler::pressed()
{
    return pressed_;
}
