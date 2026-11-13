// Control of a GoPro HERO5/6/7-generation camera over its Wi-Fi HTTP API
// ("gpControl"). Nothing about settings is hard-coded: the camera describes
// its own modes, settings and status fields in a JSON schema served at
// /gp/gpControl, and this class is driven by that schema.
#pragma once

#include "http.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace gopro {

struct Endpoint {
    std::string host = "10.5.5.9";  // fixed address of the camera on its own Wi-Fi
    int port = 80;                  // control API
    int mediaPort = 8080;           // media list and file downloads
};

struct Option {
    std::string name;  // as shown in the camera menu, e.g. "1080"
    long value = 0;    // what the camera actually stores, e.g. 9
};

struct Setting {
    int id = 0;
    std::string mode;     // "video", "photo", "multi_shot", "setup", ...
    std::string segment;  // "resolution", "fps", ...
    std::string name;     // "Resolution"
    std::vector<Option> options;

    std::string key() const { return mode + "." + segment; }
    const Option* optionByValue(long value) const;
    // Case-insensitive, ignores spaces: "superview" matches "Superview".
    const Option* optionByName(const std::string& text) const;
};

struct Mode {
    long value = 0;       // 0 video, 1 photo, 2 multi-shot, ...
    std::string segment;  // "video"
    std::string name;     // "Video"
};

struct CameraInfo {
    std::string model, firmware, serial, ssid, mac;
};

struct MediaFile {
    std::string dir;   // "100GOPRO"
    std::string name;  // "GH010123.MP4"
    std::uint64_t size = 0;
    std::time_t modified = 0;

    std::string path() const { return dir + "/" + name; }
};

// Snapshot of /gp/gpControl/status: live status values plus current settings.
struct State {
    nlohmann::json status;
    nlohmann::json settings;

    std::optional<long> statusInt(int id) const;
    std::string statusText(int id) const;
    std::optional<long> settingValue(int id) const;
};

class Camera {
public:
    explicit Camera(Endpoint endpoint = {});

    // --- schema (cached on disk, because it is ~200 kB) -------------------
    void loadSchema(bool refresh = false);
    CameraInfo info() const;
    const std::vector<Mode>& modes() const { return modes_; }
    const std::vector<Setting>& settings() const { return settings_; }
    const Setting& setting(const std::string& key) const;  // throws if unknown
    std::string modeName(long mode) const;
    std::string subModeName(long mode, long subMode) const;
    // {status id, "group.name"} for every status field the camera documents.
    const std::vector<std::pair<int, std::string>>& statusFields() const { return statusFields_; }

    // --- live state ------------------------------------------------------
    State state() const;
    // Poll until the camera is neither busy nor encoding. False on timeout.
    bool waitUntilIdle(int timeoutMs = 5000) const;

    // --- control ---------------------------------------------------------
    void shutter(bool on);
    void setMode(long mode);
    void setSubMode(long mode, long subMode);
    void applySetting(int id, long value);
    void tagMoment();
    void locate(bool on);
    void zoom(int percent);
    void syncTime();
    void sleep();
    void wake(const std::string& mac);  // Wake-on-LAN magic packet
    void completePairing(const std::string& deviceName);
    std::string raw(const std::string& pathAndQuery) const;

    // --- live preview ----------------------------------------------------
    // The camera sends an MPEG-TS stream over UDP to port 8554 of whoever
    // asked for it, for as long as keep-alive packets keep arriving.
    static constexpr int kStreamPort = 8554;
    void startStream();
    void stopStream();
    void sendStreamKeepAlive() const;  // call every ~2.5 s

    // --- media -----------------------------------------------------------
    std::vector<MediaFile> mediaList() const;
    void download(const MediaFile& file, const std::string& targetPath,
                  const ProgressFn& progress = {}) const;
    void deleteFile(const MediaFile& file);
    void deleteLast();

    const Endpoint& endpoint() const { return endpoint_; }

private:
    std::string controlUrl(const std::string& path) const;
    std::string mediaUrl(const std::string& path) const;
    void command(const std::string& path);
    // GET that keeps retrying for a few seconds while the camera answers "busy".
    std::string getWhenReady(const std::string& url, long timeoutSeconds = 10) const;
    std::string schemaCachePath() const;
    void parseSchema();

    Endpoint endpoint_;
    nlohmann::json schema_;
    std::vector<Mode> modes_;
    std::vector<Setting> settings_;
    std::vector<std::pair<int, std::string>> statusFields_;
};

}  // namespace gopro
