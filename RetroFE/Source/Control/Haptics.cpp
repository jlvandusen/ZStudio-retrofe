/* This file is part of RetroFE (ArcadeMaster extension: force feedback in the frontend). GPLv3 or later. */
#include "Haptics.h"
#include "../Database/Configuration.h"
#include "../Utility/Log.h"
#include "../Utility/Utils.h"
#include <algorithm>

Haptics& Haptics::instance() { static Haptics h; return h; }

static int pctToLevel(int pct) { return std::clamp(pct, 0, 100) * 32767 / 100; }

void Haptics::initialize(Configuration& config) {
    config.getProperty("ffb", enabled_);
    if (!enabled_) { LOG_INFO("Haptics", "frontend force feedback disabled (settings: ffb = yes to enable)"); return; }
    config.getProperty("ffbDevice", deviceMatch_);
    config.getProperty("ffbStrength", strength_);
    config.getProperty("ffbCenter", center_);
    config.getProperty("ffbDamper", damper_);
    config.getProperty("ffbTick", tick_);
    config.getProperty("ffbTickMs", tickMs_);
    config.getProperty("ffbThunk", thunkPct_);
    config.getProperty("ffbSelect", selectPct_);
    config.getProperty("ffbLaunch", launchPct_);
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0) {
        LOG_WARNING("Haptics", std::string("SDL haptic init failed: ") + SDL_GetError());
        enabled_ = false; return;
    }
    open();
}

bool Haptics::open() {
    if (!enabled_ || haptic_) return haptic_ != nullptr;
    const int n = SDL_NumHaptics();
    int pick = -1;
    const std::string want = Utils::toLower(deviceMatch_);
    for (int i = 0; i < n; ++i) {
        const char* nm = SDL_HapticName(i);
        const std::string name = Utils::toLower(nm ? nm : "");
        LOG_INFO("Haptics", "FFB device " + std::to_string(i) + ": " + (nm ? nm : "?"));
        if (!want.empty()) { if (name.find(want) != std::string::npos) { pick = i; break; } }
        else if (pick < 0 || name.find("wheel") != std::string::npos || name.find("racing") != std::string::npos) pick = i;
    }
    if (pick < 0) { LOG_WARNING("Haptics", "no matching force-feedback device (ffbDevice = '" + deviceMatch_ + "')"); return false; }

    // Prefer opening FFB *from the joystick RetroFE already has open*: on Windows the input layer holds the
    // device through DirectInput, and a second independent open cannot reset/acquire it ("Resetting device").
    const std::string pickName = Utils::toLower(SDL_HapticName(pick) ? SDL_HapticName(pick) : "");
    for (int j = 0; j < SDL_NumJoysticks() && !haptic_; ++j) {
        const char* jn = SDL_JoystickNameForIndex(j);
        const std::string jname = Utils::toLower(jn ? jn : "");
        const bool match = !want.empty() ? jname.find(want) != std::string::npos
                                         : (!pickName.empty() && (jname.find(pickName) != std::string::npos || pickName.find(jname) != std::string::npos));
        if (!match) continue;
        SDL_Joystick* joy = SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(j));
        bool openedHere = false;
        if (!joy) { joy = SDL_JoystickOpen(j); openedHere = joy != nullptr; }
        if (joy && SDL_JoystickIsHaptic(joy) == 1) {
            haptic_ = SDL_HapticOpenFromJoystick(joy);
            if (haptic_) LOG_INFO("Haptics", std::string("opened FFB from joystick '") + (jn ? jn : "?") + "'");
            else LOG_WARNING("Haptics", std::string("SDL_HapticOpenFromJoystick failed: ") + SDL_GetError());
        }
        if (!haptic_ && openedHere) SDL_JoystickClose(joy);
    }
    if (!haptic_) {
        haptic_ = SDL_HapticOpen(pick);
        if (!haptic_) { LOG_WARNING("Haptics", std::string("SDL_HapticOpen failed: ") + SDL_GetError()); return false; }
    }
    caps_ = SDL_HapticQuery(haptic_);
    if (caps_ & SDL_HAPTIC_GAIN) SDL_HapticSetGain(haptic_, std::clamp(strength_, 0, 100));

    // browsing feel: our own spring (+ damper); if the device has no spring, use its autocentre instead
    if ((caps_ & SDL_HAPTIC_SPRING) && center_ > 0) { effSpring_ = makeCondition(SDL_HAPTIC_SPRING, center_); if (caps_ & SDL_HAPTIC_AUTOCENTER) SDL_HapticSetAutocenter(haptic_, 0); }
    else if (caps_ & SDL_HAPTIC_AUTOCENTER) SDL_HapticSetAutocenter(haptic_, std::clamp(center_, 0, 100));
    if (effSpring_ < 0 && center_ > 0 && (caps_ & SDL_HAPTIC_AUTOCENTER)) {          // spring refused: device autocentre instead
        SDL_HapticSetAutocenter(haptic_, std::clamp(center_, 0, 100));
        LOG_INFO("Haptics", "using the device's autocentre (" + std::to_string(center_) + "%) for the centring feel");
    }
    if ((caps_ & SDL_HAPTIC_DAMPER) && damper_ > 0) effDamper_ = makeCondition(SDL_HAPTIC_DAMPER, damper_);
    // Menu "detent" ticks: ONE full sine period - a click felt in the hands with no net push. A one-way constant
    // pulse per scroll stepped the wheel round (the weak centring spring could not undo it between presses), and
    // with the on-screen wheel following the real one it visibly turned - the wrong way, too.
    const Uint16 tickLen = static_cast<Uint16>(std::max(20, tickMs_));
    if (caps_ & SDL_HAPTIC_SINE) { effTickFwd_ = effTickBack_ = makeSine(tick_, tickLen, tickLen, 0); }
    if (caps_ & SDL_HAPTIC_CONSTANT) {
        if (effTickFwd_ < 0) { effTickFwd_ = makeConstant(1, tick_, tickLen); effTickBack_ = makeConstant(-1, tick_, tickLen); }
        effThunk_ = makeConstant(1, thunkPct_, 90);
    }

    if (caps_ & SDL_HAPTIC_SINE) {
        effSelect_ = makeSine(selectPct_, 40, 160, 60);
        effLaunch_ = makeSine(launchPct_, 28, 450, 250);
    }
    run(effSpring_); run(effDamper_);
    LOG_INFO("Haptics", std::string("force feedback on '") + SDL_HapticName(pick) + "': spring=" + std::to_string(effSpring_ >= 0) +
             " damper=" + std::to_string(effDamper_ >= 0) + " ticks=" + std::to_string(effTickFwd_ >= 0) + " rumble=" + std::to_string(effSelect_ >= 0));
    return true;
}

void Haptics::close() {
    if (!haptic_) return;
    SDL_HapticStopAll(haptic_);
    SDL_HapticClose(haptic_);                       // destroys the effects and releases the DirectInput device
    haptic_ = nullptr;
    effSpring_ = effDamper_ = effTickFwd_ = effTickBack_ = effThunk_ = effSelect_ = effLaunch_ = -1;
}

int Haptics::makeCondition(Uint16 type, int coeffPct) {
    SDL_HapticEffect e; SDL_memset(&e, 0, sizeof(e));
    e.type = type; e.condition.type = type;
    e.condition.length = SDL_HAPTIC_INFINITY;
    // DirectInput builds one condition block per FFB axis and needs a valid direction even for springs;
    // steering wheels have a single FFB axis, so only axis 0 carries the coefficients.
    e.condition.direction.type = SDL_HAPTIC_CARTESIAN; e.condition.direction.dir[0] = 1;
    e.condition.right_sat[0] = e.condition.left_sat[0] = 0xFFFF;
    e.condition.right_coeff[0] = e.condition.left_coeff[0] = static_cast<Sint16>(pctToLevel(coeffPct));
    const int id = SDL_HapticNewEffect(haptic_, &e);
    if (id < 0) LOG_WARNING("Haptics", std::string("condition effect failed: ") + SDL_GetError());
    return id;
}

int Haptics::makeConstant(int dirX, int levelPct, Uint16 ms) {
    SDL_HapticEffect e; SDL_memset(&e, 0, sizeof(e));
    e.type = SDL_HAPTIC_CONSTANT;
    e.constant.direction.type = SDL_HAPTIC_CARTESIAN; e.constant.direction.dir[0] = dirX;
    e.constant.length = ms; e.constant.level = static_cast<Sint16>(pctToLevel(levelPct));
    e.constant.attack_length = 5; e.constant.fade_length = static_cast<Uint16>(ms / 2);
    const int id = SDL_HapticNewEffect(haptic_, &e);
    if (id < 0) LOG_WARNING("Haptics", std::string("constant effect failed: ") + SDL_GetError());
    return id;
}

int Haptics::makeSine(int magnitudePct, Uint16 periodMs, Uint16 ms, Uint16 fadeMs) {
    SDL_HapticEffect e; SDL_memset(&e, 0, sizeof(e));
    e.type = SDL_HAPTIC_SINE;
    e.periodic.direction.type = SDL_HAPTIC_CARTESIAN; e.periodic.direction.dir[0] = 1;
    e.periodic.period = periodMs; e.periodic.magnitude = static_cast<Sint16>(pctToLevel(magnitudePct));
    e.periodic.length = ms; e.periodic.fade_length = fadeMs;
    const int id = SDL_HapticNewEffect(haptic_, &e);
    if (id < 0) LOG_WARNING("Haptics", std::string("sine effect failed: ") + SDL_GetError());
    return id;
}

void Haptics::pump() {
    if (!haptic_) { tickFwd_ = tickBack_ = thunk_ = select_ = false; return; }
    const Uint32 now = SDL_GetTicks();
    if (thunk_.exchange(false)) run(effThunk_);
    if (select_.exchange(false)) run(effSelect_);
    const bool f = tickFwd_.exchange(false), b = tickBack_.exchange(false);
    if ((f || b) && now - lastTickMs_ >= static_cast<Uint32>(std::max(20, tickMs_))) {   // fast scrolls: one detent per tick length
        run(f ? effTickFwd_ : effTickBack_);
        lastTickMs_ = now;
    }
}

void Haptics::launchPulseAndRelease() {
    if (!haptic_) return;
    run(effLaunch_);
    SDL_Delay(effLaunch_ >= 0 ? 350 : 0);           // let the rumble be felt, then give the device to the game
    close();
    LOG_INFO("Haptics", "force-feedback device released for the game");
}

void Haptics::reacquire() {
    if (!enabled_ || haptic_) return;
    if (open()) LOG_INFO("Haptics", "force-feedback device re-acquired");
}

void Haptics::shutdown() { close(); }
