// ArcadeMaster: cabinet link. RetroFE tells a helper on THIS PC what is selected / started and takes simple commands
// from it (go to a game, open it). The helper (tools\AmLink) talks to the other cabinets; RetroFE itself only ever
// uses 127.0.0.1, so nothing on the network can drive it directly.
//   settings.conf  linkEvents = yes        (default no)
//                  linkSendPort = 47801    events out:  select / launch / hello, tab separated
//                  linkRecvPort = 47802    commands in: goto <item> <collection> | open
#pragma once
#include <string>
#include <cstdint>

class AmLink {
public:
    static AmLink& instance();
    void configure(bool enabled, int sendPort, int recvPort);
    bool enabled() const { return enabled_; }
    void send(const std::string& line);          // one datagram, never blocks
    bool receive(std::string& line);             // one pending command, never blocks
    void noteUserInput();                        // a real button / key / hat press on this cabinet
    double msSinceUserInput() const;             // large when nobody has touched it since start
    ~AmLink();

private:
    AmLink() = default;
    bool enabled_ = false;
    intptr_t sock_ = -1;
    int sendPort_ = 47801;
    uint64_t lastInputTicks_ = 0;
};
