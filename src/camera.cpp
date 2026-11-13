#include "camera.hpp"

#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace gopro {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// "Super View" -> "superview", so users need not match spacing or case.
std::string normalised(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (!std::isspace(c)) out += static_cast<char>(std::tolower(c));
    }
    return out;
}

std::string urlEncode(const std::string& text) {
    std::string out;
    char buffer[4];
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out += static_cast<char>(c);
        } else {
            std::snprintf(buffer, sizeof buffer, "%%%02X", c);
            out += buffer;
        }
    }
    return out;
}

// The camera sends numbers sometimes as numbers and sometimes as strings.
// 64 bits on purpose: video files pass 2 GB, and `long` is 32-bit on Windows.
std::optional<std::int64_t> toInt64(const json& value) {
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_string()) {
        try {
            return std::stoll(value.get<std::string>());
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

// Status and setting values are small, so `long` is enough for them.
std::optional<long> toLong(const json& value) {
    if (const auto wide = toInt64(value)) return static_cast<long>(*wide);
    return std::nullopt;
}

std::string toText(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "";
    return value.dump();
}

json parseJson(const std::string& body, const std::string& what) {
    json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) throw HttpError("the camera sent invalid JSON for " + what);
    return parsed;
}

}  // namespace

// --- Setting / State --------------------------------------------------------

const Option* Setting::optionByValue(long value) const {
    for (const Option& option : options) {
        if (option.value == value) return &option;
    }
    return nullptr;
}

const Option* Setting::optionByName(const std::string& text) const {
    const std::string wanted = normalised(text);
    for (const Option& option : options) {
        if (normalised(option.name) == wanted) return &option;
    }
    return nullptr;
}

std::optional<long> State::statusInt(int id) const {
    const auto it = status.find(std::to_string(id));
    return it == status.end() ? std::nullopt : toLong(*it);
}

std::string State::statusText(int id) const {
    const auto it = status.find(std::to_string(id));
    return it == status.end() ? "" : toText(*it);
}

std::optional<long> State::settingValue(int id) const {
    const auto it = settings.find(std::to_string(id));
    return it == settings.end() ? std::nullopt : toLong(*it);
}

// --- Camera: plumbing -------------------------------------------------------

Camera::Camera(Endpoint endpoint) : endpoint_(std::move(endpoint)) {}

std::string Camera::controlUrl(const std::string& path) const {
    return "http://" + endpoint_.host + ":" + std::to_string(endpoint_.port) + path;
}

std::string Camera::mediaUrl(const std::string& path) const {
    return "http://" + endpoint_.host + ":" + std::to_string(endpoint_.mediaPort) + path;
}

std::string Camera::getWhenReady(const std::string& url, long timeoutSeconds) const {
    // Right after a recording stops the camera answers 500/503 until the file
    // is saved, so "busy" is retried instead of being reported as a failure.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (true) {
        try {
            return httpGet(url, timeoutSeconds);
        } catch (const HttpError& error) {
            if (!error.cameraBusy() || std::chrono::steady_clock::now() >= deadline) throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

void Camera::command(const std::string& path) {
    getWhenReady(controlUrl("/gp/gpControl" + path));
}

std::string Camera::raw(const std::string& pathAndQuery) const {
    return httpGet(controlUrl(pathAndQuery));
}

// --- Camera: schema ---------------------------------------------------------

std::string Camera::schemaCachePath() const {
    const std::string file =
        "schema-" + endpoint_.host + "-" + std::to_string(endpoint_.port) + ".json";
    return (fs::path(platform::cacheDirectory()) / "gopro-cli" / file).string();
}

void Camera::loadSchema(bool refresh) {
    const std::string cachePath = schemaCachePath();
    if (!refresh) {
        std::ifstream cached(cachePath);
        if (cached) {
            schema_ = json::parse(cached, nullptr, false);
            if (schema_.is_object() && schema_.contains("modes")) {
                parseSchema();
                return;
            }
        }
    }
    schema_ = parseJson(httpGet(controlUrl("/gp/gpControl"), 20), "/gp/gpControl");
    if (!schema_.is_object() || !schema_.contains("modes")) {
        throw HttpError("/gp/gpControl did not return a camera schema");
    }
    parseSchema();

    std::error_code ignored;  // the cache is an optimisation; failing to write it is fine
    fs::create_directories(fs::path(cachePath).parent_path(), ignored);
    std::ofstream(cachePath) << schema_.dump();
}

void Camera::parseSchema() {
    modes_.clear();
    settings_.clear();
    statusFields_.clear();

    for (const json& mode : schema_.value("modes", json::array())) {
        const std::string modeSegment = mode.value("path_segment", "");
        modes_.push_back({mode.value("value", -1L), modeSegment, mode.value("display_name", "")});
        for (const json& entry : mode.value("settings", json::array())) {
            Setting setting;
            setting.id = entry.value("id", 0);
            setting.mode = modeSegment;
            setting.segment = entry.value("path_segment", "");
            setting.name = entry.value("display_name", "");
            for (const json& option : entry.value("options", json::array())) {
                setting.options.push_back(
                    {option.value("display_name", ""), option.value("value", 0L)});
            }
            settings_.push_back(std::move(setting));
        }
    }

    const json groups = schema_.value("status", json::object()).value("groups", json::array());
    for (const json& group : groups) {
        const std::string groupName = group.value("group", "");
        for (const json& field : group.value("fields", json::array())) {
            statusFields_.emplace_back(field.value("id", 0),
                                       groupName + "." + field.value("name", ""));
        }
    }
    std::sort(statusFields_.begin(), statusFields_.end());
}

CameraInfo Camera::info() const {
    const json info = schema_.value("info", json::object());
    CameraInfo result;
    result.model = toText(info.value("model_name", json()));
    result.firmware = toText(info.value("firmware_version", json()));
    result.serial = toText(info.value("serial_number", json()));
    result.ssid = toText(info.value("ap_ssid", json()));
    result.mac = toText(info.value("ap_mac", json()));
    return result;
}

const Setting& Camera::setting(const std::string& key) const {
    const std::string wanted = normalised(key);
    for (const Setting& candidate : settings_) {
        if (normalised(candidate.key()) == wanted) return candidate;
    }
    throw std::invalid_argument("unknown setting '" + key +
                                "' (run 'gopro settings' to list them)");
}

std::string Camera::modeName(long mode) const {
    for (const Mode& entry : modes_) {
        if (entry.value == mode) return entry.name;
    }
    return "mode " + std::to_string(mode);
}

std::string Camera::subModeName(long mode, long subMode) const {
    for (const Mode& entry : modes_) {
        if (entry.value != mode) continue;
        for (const Setting& candidate : settings_) {
            if (candidate.mode != entry.segment || candidate.segment != "current_sub_mode") continue;
            if (const Option* option = candidate.optionByValue(subMode)) return option->name;
        }
    }
    return "sub-mode " + std::to_string(subMode);
}

// --- Camera: live state -----------------------------------------------------

State Camera::state() const {
    const json body = parseJson(httpGet(controlUrl("/gp/gpControl/status")), "status");
    State result;
    result.status = body.value("status", json::object());
    result.settings = body.value("settings", json::object());
    return result;
}

bool Camera::waitUntilIdle(int timeoutMs) const {
    constexpr int kStatusBusy = 8;       // system.system_busy
    constexpr int kStatusEncoding = 10;  // system.encoding_active
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const State current = state();
        if (current.statusInt(kStatusBusy).value_or(0) == 0 &&
            current.statusInt(kStatusEncoding).value_or(0) == 0) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// --- Camera: control --------------------------------------------------------

void Camera::shutter(bool on) {
    command(std::string("/command/shutter?p=") + (on ? "1" : "0"));
}

void Camera::setMode(long mode) {
    command("/command/mode?p=" + std::to_string(mode));
}

void Camera::setSubMode(long mode, long subMode) {
    command("/command/sub_mode?mode=" + std::to_string(mode) +
            "&sub_mode=" + std::to_string(subMode));
}

void Camera::applySetting(int id, long value) {
    command("/setting/" + std::to_string(id) + "/" + std::to_string(value));
}

void Camera::tagMoment() {
    command("/command/storage/tag_moment");
}

void Camera::locate(bool on) {
    command(std::string("/command/system/locate?p=") + (on ? "1" : "0"));
}

void Camera::zoom(int percent) {
    command("/command/digital_zoom?range_pcnt=" + std::to_string(std::clamp(percent, 0, 100)));
}

void Camera::syncTime() {
    // Six bytes, each written as %HH: year-2000, month, day, hour, minute, second.
    const std::time_t now = std::time(nullptr);
    const std::tm local = platform::localTime(now);
    char encoded[32];
    std::snprintf(encoded, sizeof encoded, "%%%02x%%%02x%%%02x%%%02x%%%02x%%%02x",
                  (local.tm_year + 1900) % 100, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    command(std::string("/command/setup/date_time?p=") + encoded);
}

void Camera::sleep() {
    command("/command/system/sleep");
}

void Camera::wake(const std::string& mac) {
    // Magic packet: 6 x 0xFF followed by the MAC address repeated 16 times.
    std::vector<unsigned char> address;
    std::string digits;
    for (unsigned char c : mac) {
        if (std::isxdigit(c)) digits += static_cast<char>(c);
    }
    if (digits.size() != 12) throw std::invalid_argument("'" + mac + "' is not a MAC address");
    for (std::size_t i = 0; i < 12; i += 2) {
        address.push_back(static_cast<unsigned char>(std::stoi(digits.substr(i, 2), nullptr, 16)));
    }
    std::vector<unsigned char> packet(6, 0xFF);
    for (int i = 0; i < 16; ++i) packet.insert(packet.end(), address.begin(), address.end());

    if (!platform::sendUdp(endpoint_.host, 9, packet.data(), packet.size())) {
        throw HttpError("cannot send the wake packet to " + endpoint_.host +
                        " (not on the camera's Wi-Fi?)");
    }
}

void Camera::completePairing(const std::string& deviceName) {
    command("/command/wireless/pair/complete?success=1&deviceName=" + urlEncode(deviceName));
}

// --- Camera: live preview ---------------------------------------------------

void Camera::startStream() {
    getWhenReady(controlUrl("/gp/gpControl/execute?p1=gpStream&a1=proto_v2&c1=restart"));
}

void Camera::stopStream() {
    // Best effort with a short timeout: the camera stops streaming by itself
    // once keep-alive packets end, so this must never hold the user up.
    httpGet(controlUrl("/gp/gpControl/execute?p1=gpStream&c1=stop"), 2);
}

void Camera::sendStreamKeepAlive() const {
    static const char kMessage[] = "_GPHD_:0:0:2:0.000000\n";
    // Best effort: one lost keep-alive is harmless, the next follows in 2.5 s.
    platform::sendUdp(endpoint_.host, kStreamPort, kMessage, sizeof kMessage - 1);
}

// --- Camera: media ----------------------------------------------------------

std::vector<MediaFile> Camera::mediaList() const {
    const json body = parseJson(getWhenReady(mediaUrl("/gp/gpMediaList"), 30), "media list");
    std::vector<MediaFile> files;
    for (const json& directory : body.value("media", json::array())) {
        const std::string dirName = directory.value("d", "");
        for (const json& entry : directory.value("fs", json::array())) {
            MediaFile file;
            file.dir = dirName;
            file.name = entry.value("n", "");
            file.size = static_cast<std::uint64_t>(toInt64(entry.value("s", json())).value_or(0));
            file.modified = static_cast<std::time_t>(toInt64(entry.value("mod", json())).value_or(0));
            if (!file.name.empty()) files.push_back(std::move(file));
        }
    }
    return files;
}

void Camera::download(const MediaFile& file, const std::string& targetPath,
                      const ProgressFn& progress) const {
    httpDownload(mediaUrl("/videos/DCIM/" + file.path()), targetPath, progress);
}

void Camera::deleteFile(const MediaFile& file) {
    command("/command/storage/delete?p=/" + file.path());
}

void Camera::deleteLast() {
    command("/command/storage/delete/last");
}

}  // namespace gopro
