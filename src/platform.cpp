#include "platform.hpp"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <io.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace gopro::platform {
namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

#ifdef _WIN32
using Socket = SOCKET;
const Socket kInvalidSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }

// Winsock must be started once per process before any socket call.
bool networkReady() {
    static const bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
#else
using Socket = int;
const Socket kInvalidSocket = -1;
void closeSocket(Socket s) { close(s); }
bool networkReady() { return true; }
#endif

}  // namespace

bool sendUdp(const std::string& ipv4, int port, const void* data, std::size_t size) {
    if (!networkReady()) return false;

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, ipv4.c_str(), &target.sin_addr) != 1) return false;

    const Socket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) return false;
    // sendto returns int on Windows and ssize_t elsewhere; long long holds both.
    const long long sent = sendto(sock, static_cast<const char*>(data), static_cast<int>(size), 0,
                                  reinterpret_cast<const sockaddr*>(&target), sizeof target);
    closeSocket(sock);
    return sent == static_cast<long long>(size);
}

std::tm localTime(std::time_t when) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &when);
#else
    localtime_r(&when, &result);
#endif
    return result;
}

std::string cacheDirectory() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    if (const std::string local = env("LOCALAPPDATA"); !local.empty()) return local;
#elif defined(__APPLE__)
    if (const std::string home = env("HOME"); !home.empty()) {
        return (fs::path(home) / "Library" / "Caches").string();
    }
#else
    if (const std::string xdg = env("XDG_CACHE_HOME"); !xdg.empty()) return xdg;
    if (const std::string home = env("HOME"); !home.empty()) {
        return (fs::path(home) / ".cache").string();
    }
#endif
    std::error_code error;
    const fs::path temp = fs::temp_directory_path(error);
    return error ? "." : temp.string();
}

std::string homeDirectory() {
#ifdef _WIN32
    const std::string home = env("USERPROFILE");
#else
    const std::string home = env("HOME");
#endif
    return home.empty() ? "." : home;
}

bool stderrIsTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

}  // namespace gopro::platform
