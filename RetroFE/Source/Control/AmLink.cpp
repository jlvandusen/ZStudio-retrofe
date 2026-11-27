// ArcadeMaster: cabinet link (see AmLink.h). Plain UDP on 127.0.0.1, non-blocking.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "AmLink.h"
#include "../Utility/Log.h"
#include <SDL.h>

AmLink& AmLink::instance() {
    static AmLink link;
    return link;
}

void AmLink::configure(bool enabled, int sendPort, int recvPort) {
    if (!enabled || enabled_) return;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { LOG_WARNING("AmLink", "no sockets - link off"); return; }
#endif
    intptr_t s = static_cast<intptr_t>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s < 0) { LOG_WARNING("AmLink", "could not open a socket - link off"); return; }
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons(static_cast<unsigned short>(recvPort));
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);          // this PC only
    if (bind(static_cast<int>(s), reinterpret_cast<sockaddr*>(&me), sizeof(me)) != 0) {
        LOG_WARNING("AmLink", "port " + std::to_string(recvPort) + " is in use - link off");
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(s));
#else
        close(static_cast<int>(s));
#endif
        return;
    }
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &nb);
#else
    fcntl(static_cast<int>(s), F_SETFL, fcntl(static_cast<int>(s), F_GETFL, 0) | O_NONBLOCK);
#endif
    sock_ = s; sendPort_ = sendPort; enabled_ = true;
    LOG_INFO("AmLink", "cabinet link on: events to 127.0.0.1:" + std::to_string(sendPort) + ", commands on 127.0.0.1:" + std::to_string(recvPort));
}

void AmLink::send(const std::string& line) {
    if (!enabled_) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<unsigned short>(sendPort_));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(static_cast<int>(sock_), line.c_str(), static_cast<int>(line.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

bool AmLink::receive(std::string& line) {
    if (!enabled_) return false;
    char buf[1024];
    sockaddr_in from{}; socklen_t fl = sizeof(from);
    int n = static_cast<int>(recvfrom(static_cast<int>(sock_), buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl));
    if (n <= 0) return false;
    if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return false;   // belt and braces: only this PC
    line.assign(buf, static_cast<size_t>(n));
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return !line.empty();
}

void AmLink::noteUserInput() { lastInputTicks_ = SDL_GetPerformanceCounter(); }

double AmLink::msSinceUserInput() const {
    if (lastInputTicks_ == 0) return 1e12;
    return static_cast<double>(SDL_GetPerformanceCounter() - lastInputTicks_) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
}

AmLink::~AmLink() {
    if (sock_ >= 0) {
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(sock_));
#else
        close(static_cast<int>(sock_));
#endif
    }
}
