// Everything that differs between Linux, macOS and Windows lives here, so the
// rest of the code is plain standard C++.
#pragma once

#include <ctime>
#include <string>

namespace gopro::platform {

// Send one UDP datagram to a numeric IPv4 address. False if it was not sent.
bool sendUdp(const std::string& ipv4, int port, const void* data, std::size_t size);

// Thread-safe replacement for std::localtime.
std::tm localTime(std::time_t when);

// Where the settings schema is cached:
//   Linux    $XDG_CACHE_HOME or ~/.cache
//   macOS    ~/Library/Caches
//   Windows  %LOCALAPPDATA%
std::string cacheDirectory();

// The user's home folder (%USERPROFILE% on Windows); "." if unknown.
std::string homeDirectory();

// True when stderr is an interactive terminal (so a progress line makes sense).
bool stderrIsTerminal();

}  // namespace gopro::platform
