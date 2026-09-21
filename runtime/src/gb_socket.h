// Private C++17 host socket helpers. No guest state or platform UI dependency.
#ifndef GBRT_SOCKET_H
#define GBRT_SOCKET_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <climits>
#include <cstring>
#include <string>

namespace gb_net {
#ifdef _WIN32
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket invalid = INVALID_SOCKET;
#else
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket invalid = -1;
#endif

class Environment {
    bool active_ = false;

  public:
    bool start() {
        if (active_)
            return true;
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return false;
#endif
        active_ = true;
        return true;
    }
    void stop() {
        if (!active_)
            return;
#ifdef _WIN32
        WSACleanup();
#endif
        active_ = false;
    }
    ~Environment() { stop(); }
};

inline bool interrupted() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}
inline std::string error() {
#ifdef _WIN32
    return "Winsock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}
inline void close(Socket fd) {
    if (fd == invalid)
        return;
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}
inline int option(Socket fd, int level, int name, const void *data, int size) {
    return setsockopt(fd, level, name, static_cast<const char *>(data), size);
}
inline bool timeout(Socket fd, int name, unsigned milliseconds) {
#ifdef _WIN32
    DWORD value = milliseconds;
#else
    timeval value{};
    value.tv_sec = milliseconds / 1000;
    value.tv_usec = (milliseconds % 1000) * 1000;
#endif
    return option(fd, SOL_SOCKET, name, &value, sizeof(value)) == 0;
}
// Bound cancellation without closing a socket still owned by a worker thread.
inline int readable(Socket fd, unsigned milliseconds) {
#ifdef _WIN32
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(fd, &read_set);
    timeval value{};
    value.tv_sec = milliseconds / 1000;
    value.tv_usec = (milliseconds % 1000) * 1000;
    return select(0, &read_set, nullptr, nullptr, &value);
#else
    pollfd descriptor{fd, POLLIN, 0};
    return poll(&descriptor, 1, static_cast<int>(milliseconds));
#endif
}
inline int send(Socket fd, const void *data, size_t size) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    const int count = size > INT_MAX ? INT_MAX : static_cast<int>(size);
    return static_cast<int>(::send(fd, static_cast<const char *>(data), count, flags));
}
inline int receive(Socket fd, void *data, size_t size) {
    const int count = size > INT_MAX ? INT_MAX : static_cast<int>(size);
    return static_cast<int>(recv(fd, static_cast<char *>(data), count, 0));
}
} // namespace gb_net
#endif
