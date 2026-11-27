/* This file is part of RetroFE (ArcadeMaster extension: force feedback in the frontend). GPLv3 or later. */
#pragma once
#include <SDL.h>
#include <atomic>
#include <string>

class Configuration;

// Force feedback for the frontend itself (steering wheels / FFB joysticks via SDL haptics = DirectInput on Windows).
//
//   browsing : soft centring spring + light damper, so the wheel feels "alive" instead of limp
//   scroll   : a short detent tick per item, pushed in the scroll direction
//   enter    : a heavier "gear change" thunk when a collection opens
//   select   : a short rumble
//   launch   : a rumble, then the device is RELEASED - DirectInput FFB is exclusive, so a game
//              (TeknoParrot FFB plugin, emulators) must get the wheel to itself - and re-acquired on return
//
// Thread-safe by design: request*() only set atomic flags (Page::scroll may run on worker threads);
// pump() executes them on the main thread once per frame.
//
// settings.conf (all optional; ffb defaults to off):
//   ffb = yes | ffbDevice = G29 (name substring; default: first wheel-like FFB device)
//   ffbStrength = 75 (overall gain %) | ffbCenter = 35 | ffbDamper = 12 | ffbTick = 45 | ffbTickMs = 35
//   ffbThunk = 60 | ffbSelect = 55 | ffbLaunch = 85
class Haptics {
public:
    static Haptics& instance();
    void initialize(Configuration& config);
    void pump();                                    // main thread, once per frame
    void requestTick(bool forward) { (forward ? tickFwd_ : tickBack_).store(true); }
    void requestThunk() { thunk_.store(true); }
    void requestSelect() { select_.store(true); }
    void launchPulseAndRelease();                   // main thread: rumble, then hand the device to the game
    void reacquire();                               // main thread: after the game exits
    void shutdown();
    bool active() const { return haptic_ != nullptr; }

private:
    Haptics() = default;
    bool open();
    void close();
    int  makeConstant(int dirX, int levelPct, Uint16 ms);
    int  makeSine(int magnitudePct, Uint16 periodMs, Uint16 ms, Uint16 fadeMs);
    int  makeCondition(Uint16 type, int coeffPct);
    void run(int id) { if (haptic_ && id >= 0) SDL_HapticRunEffect(haptic_, id, 1); }

    bool enabled_ = false;
    int openRetries_ = 0; Uint32 lastOpenTryMs_ = 0;   // ArcadeMaster: pump() keeps trying to open the device
    std::string deviceMatch_;
    int strength_ = 75, center_ = 35, damper_ = 12, tick_ = 0 /* menu clicks off by default: on wheels they stepped the rim round (ffbTick = 45 to try) */, tickMs_ = 35, thunkPct_ = 60, selectPct_ = 55, launchPct_ = 85;

    SDL_Haptic* haptic_ = nullptr;
    unsigned int caps_ = 0;
    int effSpring_ = -1, effDamper_ = -1, effTickFwd_ = -1, effTickBack_ = -1, effThunk_ = -1, effSelect_ = -1, effLaunch_ = -1;
    Uint32 lastTickMs_ = 0;

    std::atomic<bool> tickFwd_{ false }, tickBack_{ false }, thunk_{ false }, select_{ false };
};
