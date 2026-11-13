// Runs every camera request on a background thread, so the window never
// freezes while the camera is slow or busy. The UI reads a snapshot of the
// latest known state and posts tasks; it never talks to the camera directly.
#pragma once

#include "camera.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Snapshot {
    bool connected = false;
    bool haveSchema = false;
    std::string error;    // why we are not connected, or the last failed action
    std::string message;  // last successful action, shown in the status bar

    gopro::CameraInfo info;
    std::vector<gopro::Mode> modes;
    std::vector<gopro::Setting> settings;
    gopro::State state;

    bool mediaLoaded = false;
    bool mediaLoading = false;
    std::vector<gopro::MediaFile> media;

    bool downloading = false;
    std::string downloadName;
    std::uint64_t downloadNow = 0, downloadTotal = 0;
};

class Worker {
public:
    using Task = std::function<void(gopro::Camera&)>;

    explicit Worker(gopro::Endpoint endpoint);
    ~Worker();

    Snapshot snapshot() const;

    // Queue a camera action. `label` appears in the status bar when it is done.
    void post(const std::string& label, Task task);
    void refreshMedia();
    void download(const gopro::MediaFile& file, const std::string& directory);
    void setPreviewWanted(bool wanted) { previewWanted_ = wanted; }
    void restartPreview() { previewStarted_ = false; }

private:
    void run();
    void pollState();

    gopro::Endpoint endpoint_;
    gopro::Camera camera_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<std::string, Task>> tasks_;
    Snapshot shared_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> previewWanted_{true};
    std::atomic<bool> previewStarted_{false};
    std::thread thread_;
    std::thread downloadThread_;
};
