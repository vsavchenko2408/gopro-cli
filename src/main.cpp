// gopro — command-line control of a GoPro HERO7 Black (and other
// gpControl-generation cameras) over Wi-Fi. Runs on Linux, macOS and Windows.

#include "camera.hpp"
#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef GOPRO_VERSION
#  define GOPRO_VERSION "dev"
#endif

namespace {

using gopro::Camera;
using gopro::MediaFile;
using gopro::Setting;
using gopro::State;

// Status ids used for the summary; names come from the camera's own schema.
constexpr int kBatteryPercent = 70;
constexpr int kBatteryBars = 2;
constexpr int kBusy = 8;
constexpr int kEncoding = 10;
constexpr int kVideoProgress = 13;
constexpr int kSdStatus = 33;
constexpr int kRemainingPhotos = 34;
constexpr int kRemainingVideoSeconds = 35;
constexpr int kTotalPhotos = 38;
constexpr int kTotalVideos = 39;
constexpr int kMode = 43;
constexpr int kSubMode = 44;
constexpr int kRemainingSpaceKb = 54;

const char* const kUsage = R"(gopro - control a GoPro over Wi-Fi

Usage: gopro [options] <command> [arguments]

Camera
  info                       model, firmware, serial number
  status [--all]             battery, mode, SD card (--all: every status field)
  pair [name]                finish pairing started in the camera's Connections menu
  time                       set the camera clock from this computer
  locate on|off              make the camera beep so you can find it
  sleep                      power the camera off
  wake [mac]                 power it back on (Wake-on-LAN)

Shooting
  record start|stop          switch to video and start, or stop recording
  photo                      switch to photo and take one
  shutter on|off             press / release the shutter in the current mode
  mode [mode [sub-mode]]     show modes, or switch: gopro mode video looping
  preview [--player cmd]     live picture from the camera in a window (needs ffplay)
  tag                        add a HiLight tag while recording
  zoom <0-100>               digital zoom, percent

Settings
  settings [filter]          list settings and current values: gopro settings video
  get <setting>              show one setting and its options: gopro get video.fps
  set <setting> <option>     change it: gopro set video.resolution 1080

Media
  media ls                   list files on the SD card
  media get <name|last|all> [-o dir]   download
  media rm <name|last> [--yes]         delete from the SD card

Advanced
  raw <path>                 send any request: gopro raw /gp/gpControl/status

Options
  --host <ip>          camera address (default 10.5.5.9, or $GOPRO_HOST)
  --port <n>           control port (default 80, or $GOPRO_PORT)
  --media-port <n>     media port (default 8080, or $GOPRO_MEDIA_PORT)
  --refresh            re-read the settings schema from the camera
  -v, --verbose        print every request sent to the camera and its answer
  --version            print the version
  -h, --help           this text
)";

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

int toInt(const std::string& text, const std::string& what) {
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used == text.size()) return value;
    } catch (const std::exception&) {
    }
    throw UsageError(what + " must be a number, got '" + text + "'");
}

bool toOnOff(const std::string& text) {
    const std::string word = lower(text);
    if (word == "on" || word == "1" || word == "start") return true;
    if (word == "off" || word == "0" || word == "stop") return false;
    throw UsageError("expected on or off, got '" + text + "'");
}

std::string humanSize(double bytes) {
    const char* units[] = {"B", "kB", "MB", "GB", "TB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) {
        bytes /= 1024.0;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof text, unit == 0 ? "%.0f %s" : "%.1f %s", bytes, units[unit]);
    return text;
}

std::string humanDuration(long seconds) {
    char text[32];
    if (seconds >= 3600) {
        std::snprintf(text, sizeof text, "%ldh %02ldm", seconds / 3600, seconds % 3600 / 60);
    } else {
        std::snprintf(text, sizeof text, "%ldm %02lds", seconds / 60, seconds % 60);
    }
    return text;
}

std::string currentOptionName(const Setting& setting, const State& state) {
    const auto value = state.settingValue(setting.id);
    if (!value) return "(not reported)";
    if (const gopro::Option* option = setting.optionByValue(*value)) return option->name;
    return "value " + std::to_string(*value);
}

// The first argument, removed from the list; throws if it is missing.
std::string take(std::vector<std::string>& args, const std::string& what) {
    if (args.empty()) throw UsageError("missing " + what);
    std::string first = args.front();
    args.erase(args.begin());
    return first;
}

// Remove a flag such as --all from the arguments; true if it was present.
bool takeFlag(std::vector<std::string>& args, const std::string& flag) {
    const auto it = std::find(args.begin(), args.end(), flag);
    if (it == args.end()) return false;
    args.erase(it);
    return true;
}

// --- commands ---------------------------------------------------------------

void cmdInfo(Camera& camera) {
    const gopro::CameraInfo info = camera.info();
    std::cout << "Model:     " << info.model << "\n"
              << "Firmware:  " << info.firmware << "\n"
              << "Serial:    " << info.serial << "\n"
              << "Wi-Fi:     " << info.ssid << "\n"
              << "MAC:       " << info.mac << "\n";
}

void cmdStatus(Camera& camera, std::vector<std::string>& args) {
    const bool all = takeFlag(args, "--all");
    const State state = camera.state();

    if (all) {
        for (const auto& [id, name] : camera.statusFields()) {
            const std::string value = state.statusText(id);
            if (!value.empty()) std::printf("%3d  %-40s %s\n", id, name.c_str(), value.c_str());
        }
        return;
    }

    const long mode = state.statusInt(kMode).value_or(-1);
    const long subMode = state.statusInt(kSubMode).value_or(-1);
    std::cout << "Mode:       " << camera.modeName(mode) << " / "
              << camera.subModeName(mode, subMode) << "\n";

    const bool encoding = state.statusInt(kEncoding).value_or(0) != 0;
    std::cout << "Recording:  " << (encoding ? "yes" : "no");
    if (encoding) {
        std::cout << " (" << humanDuration(state.statusInt(kVideoProgress).value_or(0)) << ")";
    }
    std::cout << "\n";
    if (state.statusInt(kBusy).value_or(0) != 0) std::cout << "Busy:       yes\n";

    if (const auto percent = state.statusInt(kBatteryPercent)) {
        std::cout << "Battery:    " << *percent << "%\n";
    } else if (const auto bars = state.statusInt(kBatteryBars)) {
        std::cout << "Battery:    level " << *bars << " (0 empty .. 3 full, 4 charging)\n";
    }

    if (state.statusInt(kSdStatus).value_or(0) != 0) {
        std::cout << "SD card:    not ready (status " << state.statusText(kSdStatus) << ")\n";
    } else {
        // The camera reports free space in kilobytes.
        const double freeBytes = 1024.0 * state.statusInt(kRemainingSpaceKb).value_or(0);
        std::cout << "SD card:    " << humanSize(freeBytes) << " free, room for "
                  << humanDuration(state.statusInt(kRemainingVideoSeconds).value_or(0))
                  << " of video or " << state.statusInt(kRemainingPhotos).value_or(0)
                  << " photos\n";
        std::cout << "On card:    " << state.statusInt(kTotalVideos).value_or(0) << " videos, "
                  << state.statusInt(kTotalPhotos).value_or(0) << " photos\n";
    }
}

// "multi_shot", "Multi-Shot" and "multishot" all compare equal.
std::string squash(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
    }
    return out;
}

// Modes the "mode" command can switch to: video (0), photo (1), multi-shot (2).
std::vector<gopro::Mode> switchableModes(const Camera& camera) {
    std::vector<gopro::Mode> modes;
    for (const gopro::Mode& mode : camera.modes()) {
        if (mode.value >= 0 && mode.value <= 2) modes.push_back(mode);
    }
    return modes;
}

void cmdMode(Camera& camera, std::vector<std::string>& args) {
    const std::vector<gopro::Mode> modes = switchableModes(camera);

    if (args.empty()) {
        for (const gopro::Mode& mode : modes) {
            std::cout << mode.segment << ":";
            for (const gopro::Option& sub : camera.setting(mode.segment + ".current_sub_mode").options) {
                std::cout << "  \"" << sub.name << "\"";
            }
            std::cout << "\n";
        }
        return;
    }

    const std::string wanted = squash(take(args, "mode"));
    for (const gopro::Mode& mode : modes) {
        if (squash(mode.segment) != wanted) continue;

        if (args.empty()) {
            camera.setMode(mode.value);
            std::cout << "Mode: " << mode.name << "\n";
            return;
        }
        // Remaining words form the sub-mode name: gopro mode video time warp video
        std::string subName;
        for (const std::string& word : args) subName += word;
        const Setting& subModes = camera.setting(mode.segment + ".current_sub_mode");
        const gopro::Option* sub = subModes.optionByName(subName);
        if (!sub) {
            // Accept a unique prefix too, so "looping" or "timewarp" is enough.
            const std::string prefix = squash(subName);
            for (const gopro::Option& option : subModes.options) {
                if (squash(option.name).rfind(prefix, 0) == 0) {
                    if (sub) throw UsageError("'" + subName + "' matches several sub-modes");
                    sub = &option;
                }
            }
        }
        if (!sub) throw UsageError("unknown sub-mode (run 'gopro mode' to list them)");
        camera.setSubMode(mode.value, sub->value);
        std::cout << "Mode: " << mode.name << " / " << sub->name << "\n";
        return;
    }
    throw UsageError("unknown mode '" + wanted + "' (run 'gopro mode' to list them)");
}

// Poll the camera until `done(state)` is true. False if it never became true.
template <typename Predicate>
bool waitFor(const Camera& camera, int timeoutMs, Predicate done) {
    for (int waited = 0;; waited += 250) {
        if (done(camera.state())) return true;
        if (waited >= timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// Switch the primary mode and confirm the camera really did it.
void switchMode(Camera& camera, long mode) {
    if (camera.state().statusInt(kMode).value_or(-1) == mode) return;
    camera.setMode(mode);
    const bool switched = waitFor(camera, 4000, [&](const State& state) {
        return state.statusInt(kMode).value_or(-1) == mode && state.statusInt(kBusy).value_or(0) == 0;
    });
    if (!switched) {
        const long actual = camera.state().statusInt(kMode).value_or(-1);
        throw std::runtime_error("the camera did not switch to " + camera.modeName(mode) +
                                 " (it reports " + camera.modeName(actual) + ")");
    }
}

void cmdRecord(Camera& camera, std::vector<std::string>& args) {
    const auto isEncoding = [](const State& state) {
        return state.statusInt(kEncoding).value_or(0) != 0;
    };
    if (toOnOff(take(args, "start or stop"))) {
        if (isEncoding(camera.state())) {
            std::cout << "Already recording\n";
            return;
        }
        switchMode(camera, 0);  // 0 = video
        camera.shutter(true);
        if (!waitFor(camera, 4000, isEncoding)) {
            throw std::runtime_error("the camera accepted the command but is not recording");
        }
        std::cout << "Recording started\n";
    } else {
        if (!isEncoding(camera.state())) {
            std::cout << "Not recording\n";
            return;
        }
        camera.shutter(false);
        camera.waitUntilIdle(10000);  // let the camera finish saving the file
        std::cout << "Recording stopped\n";
    }
}

void cmdPhoto(Camera& camera) {
    const State before = camera.state();
    if (before.statusInt(kEncoding).value_or(0) != 0) {
        throw std::runtime_error("the camera is recording; stop it first (gopro record stop)");
    }
    const long photosBefore = before.statusInt(kTotalPhotos).value_or(0);
    switchMode(camera, 1);  // 1 = photo
    camera.shutter(true);
    // Night photos can expose for many seconds, so be patient.
    const bool saved = waitFor(camera, 40000, [&](const State& state) {
        return state.statusInt(kTotalPhotos).value_or(0) > photosBefore;
    });
    if (!saved) {
        throw std::runtime_error("shutter pressed, but no new photo appeared on the SD card");
    }
    std::cout << "Photo taken\n";
}

volatile std::sig_atomic_t interrupted = 0;

void cmdPreview(Camera& camera, std::vector<std::string>& args) {
    // The player listens on UDP 8554. It must read enough of the stream to see
    // an H.264 key frame (-probesize), otherwise it never learns the picture
    // size and shows no video. Audio is dropped (-an): it is not needed for
    // framing a shot and syncing to it adds delay.
    std::string player = "ffplay -loglevel error -fflags nobuffer -flags low_delay -framedrop "
                         "-an -probesize 400000 -analyzeduration 2000000 -f mpegts "
                         "-window_title \"GoPro preview\"";
    if (const auto it = std::find(args.begin(), args.end(), "--player"); it != args.end()) {
        if (it + 1 == args.end()) throw UsageError("--player needs a command");
        player = *(it + 1);
    }
    const std::string commandLine =
        player + " udp://0.0.0.0:" + std::to_string(Camera::kStreamPort);

    camera.startStream();
    camera.sendStreamKeepAlive();

    // The player runs through the system shell on its own thread while this
    // thread keeps the stream alive. Ctrl+C reaches the player as well, so
    // the loop ends when the player does.
    std::atomic<bool> playerDone{false};
    int playerStatus = 0;
    const auto started = std::chrono::steady_clock::now();
    std::thread playerThread([&] {
        playerStatus = std::system(commandLine.c_str());
        playerDone = true;
    });

    std::cout << "Preview running. Close the player window or press Ctrl+C to stop.\n";
    std::signal(SIGINT, [](int) { interrupted = 1; });
    for (int tick = 0; !playerDone; ++tick) {
        if (tick % 25 == 0) camera.sendStreamKeepAlive();  // every 2.5 s
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    playerThread.join();
    std::signal(SIGINT, SIG_DFL);

    try {
        camera.stopStream();
    } catch (const gopro::HttpError&) {
        // The camera stops by itself once keep-alives end; nothing to recover.
    }
    const auto ran = std::chrono::steady_clock::now() - started;
    if (playerStatus != 0 && !interrupted && ran < std::chrono::seconds(2)) {
        throw std::runtime_error("the video player could not be started; "
                                 "ffplay is part of FFmpeg (https://ffmpeg.org)");
    }
}

void cmdSettings(Camera& camera, std::vector<std::string>& args) {
    const std::string filter = args.empty() ? "" : lower(args.front());
    const State state = camera.state();
    for (const Setting& setting : camera.settings()) {
        const std::string key = setting.key();
        if (!filter.empty() && lower(key + " " + setting.name).find(filter) == std::string::npos) {
            continue;
        }
        std::printf("%-36s %-22s %s\n", key.c_str(), currentOptionName(setting, state).c_str(),
                    setting.name.c_str());
    }
}

void cmdGet(Camera& camera, std::vector<std::string>& args) {
    const Setting& setting = camera.setting(take(args, "setting name"));
    const State state = camera.state();
    const auto current = state.settingValue(setting.id);
    std::cout << setting.key() << " - " << setting.name << " (id " << setting.id << ")\n";
    for (const gopro::Option& option : setting.options) {
        std::cout << (current && *current == option.value ? "  * " : "    ") << option.name << "\n";
    }
}

void cmdSet(Camera& camera, std::vector<std::string>& args) {
    const Setting& setting = camera.setting(take(args, "setting name"));
    std::string wanted;
    for (const std::string& word : args) wanted += word;
    if (wanted.empty()) throw UsageError("missing option (run 'gopro get " + setting.key() + "')");

    // Sliders such as setup.lcd_brightness_v2 take any number, not only the
    // listed options, so a plain number is sent as-is when no option matches.
    gopro::Option numeric;
    const gopro::Option* option = setting.optionByName(wanted);
    if (!option && !wanted.empty() &&
        std::all_of(wanted.begin(), wanted.end(), [](unsigned char c) { return std::isdigit(c); })) {
        numeric = {wanted, toInt(wanted, "value")};
        option = &numeric;
    }
    if (!option) {
        throw UsageError("'" + wanted + "' is not an option of " + setting.key() +
                         " (run 'gopro get " + setting.key() + "')");
    }
    camera.applySetting(setting.id, option->value);

    // The camera may silently refuse a combination, so read the value back.
    camera.waitUntilIdle(2000);
    const State state = camera.state();
    if (state.settingValue(setting.id).value_or(option->value - 1) != option->value) {
        throw std::runtime_error("the camera did not accept " + option->name + "; " +
                                 setting.key() + " is still " + currentOptionName(setting, state) +
                                 " (it may not be available with the other current settings)");
    }
    std::cout << setting.key() << " = " << option->name << "\n";
}

std::string formatTime(std::time_t when) {
    const std::tm local = gopro::platform::localTime(when);
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local);
    return text;
}

MediaFile findFile(const std::vector<MediaFile>& files, const std::string& name) {
    const std::string wanted = lower(name);
    for (const MediaFile& file : files) {
        if (lower(file.name) == wanted || lower(file.path()) == wanted) return file;
    }
    throw UsageError("no file named '" + name + "' on the SD card");
}

void downloadOne(const Camera& camera, const MediaFile& file, const std::string& directory) {
    namespace fs = std::filesystem;
    const fs::path target = fs::path(directory) / file.name;
    std::error_code error;
    if (fs::exists(target, error) && fs::file_size(target, error) == file.size) {
        std::cout << file.name << ": already downloaded\n";
        return;
    }
    const bool interactive = gopro::platform::stderrIsTerminal();
    int lastPercent = -1;
    camera.download(file, target.string(), [&](std::uint64_t now, std::uint64_t total) {
        if (!interactive || total == 0) return;
        const int percent = static_cast<int>(now * 100 / total);
        if (percent == lastPercent) return;
        lastPercent = percent;
        std::fprintf(stderr, "\r%s: %3d%% of %s", file.name.c_str(), percent,
                     humanSize(static_cast<double>(total)).c_str());
    });
    if (interactive) std::fprintf(stderr, "\r\033[K");
    std::cout << file.name << ": saved to " << target.string() << " ("
              << humanSize(static_cast<double>(file.size)) << ")\n";
}

void cmdMedia(Camera& camera, std::vector<std::string>& args) {
    const std::string action = lower(take(args, "ls, get or rm"));

    if (action == "ls" || action == "list") {
        std::uint64_t total = 0;
        const std::vector<MediaFile> files = camera.mediaList();
        for (const MediaFile& file : files) {
            total += file.size;
            std::printf("%-24s %10s  %s\n", file.path().c_str(),
                        humanSize(static_cast<double>(file.size)).c_str(),
                        formatTime(file.modified).c_str());
        }
        std::cout << files.size() << " files, " << humanSize(static_cast<double>(total)) << "\n";
        return;
    }

    if (action == "get") {
        std::string directory = ".";
        if (const auto it = std::find(args.begin(), args.end(), "-o"); it != args.end()) {
            if (it + 1 == args.end()) throw UsageError("-o needs a directory");
            directory = *(it + 1);
            args.erase(it, it + 2);
        }
        const std::string what = take(args, "file name, 'last' or 'all'");
        const std::vector<MediaFile> files = camera.mediaList();
        if (files.empty()) throw std::runtime_error("the SD card has no media");
        std::filesystem::create_directories(directory);
        if (lower(what) == "all") {
            for (const MediaFile& file : files) downloadOne(camera, file, directory);
        } else if (lower(what) == "last") {
            downloadOne(camera, files.back(), directory);
        } else {
            downloadOne(camera, findFile(files, what), directory);
        }
        return;
    }

    if (action == "rm" || action == "delete") {
        const bool yes = takeFlag(args, "--yes") || takeFlag(args, "-y");
        const std::string what = take(args, "file name or 'last'");
        const std::vector<MediaFile> files = camera.mediaList();
        if (files.empty()) throw std::runtime_error("the SD card has no media");
        const MediaFile file = lower(what) == "last" ? files.back() : findFile(files, what);
        if (!yes) {
            std::cout << "Delete " << file.path() << " from the SD card? This cannot be undone. [y/N] "
                      << std::flush;
            std::string answer;
            std::getline(std::cin, answer);
            if (lower(answer) != "y" && lower(answer) != "yes") {
                std::cout << "Nothing deleted\n";
                return;
            }
        }
        camera.deleteFile(file);
        std::cout << "Deleted " << file.path() << "\n";
        return;
    }

    throw UsageError("unknown media action '" + action + "'");
}

int envInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value && *value ? toInt(value, name) : fallback;
}

int run(std::vector<std::string> args) {
    gopro::Endpoint endpoint;
    if (const char* host = std::getenv("GOPRO_HOST"); host && *host) endpoint.host = host;
    endpoint.port = envInt("GOPRO_PORT", endpoint.port);
    endpoint.mediaPort = envInt("GOPRO_MEDIA_PORT", endpoint.mediaPort);

    // Global options may appear anywhere on the command line.
    bool refresh = false;
    for (std::size_t i = 0; i < args.size();) {
        const std::string& arg = args[i];
        const bool hasValue = i + 1 < args.size();
        if (arg == "-h" || arg == "--help") {
            std::cout << kUsage;
            return 0;
        } else if (arg == "--version") {
            std::cout << "gopro " << GOPRO_VERSION << "\n";
            return 0;
        } else if (arg == "-v" || arg == "--verbose") {
            gopro::setHttpTrace(true);
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (arg == "--refresh") {
            refresh = true;
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (arg == "--host" || arg == "--port" || arg == "--media-port") {
            if (!hasValue) throw UsageError(arg + " needs a value");
            if (arg == "--host") endpoint.host = args[i + 1];
            if (arg == "--port") endpoint.port = toInt(args[i + 1], arg);
            if (arg == "--media-port") endpoint.mediaPort = toInt(args[i + 1], arg);
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                       args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        } else {
            ++i;
        }
    }
    if (args.empty()) {
        std::cout << kUsage;
        return 2;
    }

    Camera camera(endpoint);
    const std::string command = lower(take(args, "command"));

    // Commands that work without the settings schema.
    if (command == "shutter") {
        const bool on = toOnOff(take(args, "on or off"));
        camera.shutter(on);
        std::cout << (on ? "Shutter pressed\n" : "Shutter released\n");
    } else if (command == "tag") {
        camera.tagMoment();
        std::cout << "HiLight tag added\n";
    } else if (command == "locate") {
        const bool on = toOnOff(take(args, "on or off"));
        camera.locate(on);
        std::cout << (on ? "Camera is beeping\n" : "Beeping stopped\n");
    } else if (command == "zoom") {
        const int percent = std::clamp(toInt(take(args, "zoom percent"), "zoom"), 0, 100);
        camera.zoom(percent);
        std::cout << "Zoom: " << percent << "%\n";
    } else if (command == "time") {
        camera.syncTime();
        std::cout << "Camera clock set to " << formatTime(std::time(nullptr)) << "\n";
    } else if (command == "sleep") {
        camera.sleep();
        std::cout << "Camera is powering off\n";
    } else if (command == "pair") {
        camera.completePairing(args.empty() ? "gopro-cli" : args.front());
        std::cout << "Pairing completed\n";
    } else if (command == "raw") {
        std::cout << camera.raw(take(args, "path")) << "\n";
    } else if (command == "preview") {
        cmdPreview(camera, args);
    } else if (command == "media") {
        cmdMedia(camera, args);
    } else if (command == "wake") {
        std::string mac = args.empty() ? "" : args.front();
        if (mac.empty()) {
            // The MAC address is in the cached schema from an earlier session.
            try {
                camera.loadSchema();
                mac = camera.info().mac;
            } catch (const gopro::HttpError&) {
                throw UsageError("no cached MAC address yet; pass it: gopro wake <mac>");
            }
        }
        camera.wake(mac);
        std::cout << "Wake packet sent to " << mac << "\n";
    } else {
        // Everything else needs the schema.
        camera.loadSchema(refresh);
        if (command == "info") {
            cmdInfo(camera);
        } else if (command == "status") {
            cmdStatus(camera, args);
        } else if (command == "mode") {
            cmdMode(camera, args);
        } else if (command == "record") {
            cmdRecord(camera, args);
        } else if (command == "photo") {
            cmdPhoto(camera);
        } else if (command == "settings") {
            cmdSettings(camera, args);
        } else if (command == "get") {
            cmdGet(camera, args);
        } else if (command == "set") {
            cmdSet(camera, args);
        } else {
            throw UsageError("unknown command '" + command + "' (try gopro --help)");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const UsageError& error) {
        std::cerr << "gopro: " << error.what() << "\n";
        return 2;
    } catch (const gopro::HttpError& error) {
        std::cerr << "gopro: " << error.what() << "\n";
        if (error.cameraBusy()) {
            std::cerr << "The camera is still busy. Wait a few seconds and try again.\n";
        } else if (error.httpStatus() == 0) {
            std::cerr << "Check that the camera is on and this computer is connected to its Wi-Fi.\n";
        }
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "gopro: " << error.what() << "\n";
        return 1;
    }
}
