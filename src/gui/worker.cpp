#include "worker.hpp"

#include <chrono>
#include <filesystem>

using Clock = std::chrono::steady_clock;

Worker::Worker(gopro::Endpoint endpoint) : endpoint_(endpoint), camera_(endpoint) {
    thread_ = std::thread(&Worker::run, this);
}

Worker::~Worker() {
    stop_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (downloadThread_.joinable()) downloadThread_.join();
}

Snapshot Worker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shared_;
}

void Worker::post(const std::string& label, Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.emplace_back(label, std::move(task));
    }
    wake_.notify_all();
}

void Worker::refreshMedia() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_.mediaLoading) return;
        shared_.mediaLoading = true;
    }
    post("", [this](gopro::Camera& camera) {
        std::vector<gopro::MediaFile> files;
        try {
            files = camera.mediaList();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            shared_.mediaLoading = false;
            throw;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        shared_.media = std::move(files);
        shared_.mediaLoaded = true;
        shared_.mediaLoading = false;
    });
}

void Worker::download(const gopro::MediaFile& file, const std::string& directory) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_.downloading) return;  // one download at a time
        shared_.downloading = true;
        shared_.downloadName = file.name;
        shared_.downloadNow = 0;
        shared_.downloadTotal = file.size;
    }
    if (downloadThread_.joinable()) downloadThread_.join();
    // Downloads get their own thread and connection, so status polling and
    // the controls keep working while a large video is being copied.
    downloadThread_ = std::thread([this, file, directory] {
        std::string result;
        try {
            std::filesystem::create_directories(directory);
            const std::string target = (std::filesystem::path(directory) / file.name).string();
            gopro::Camera(endpoint_).download(file, target,
                                              [this](std::uint64_t now, std::uint64_t total) {
                std::lock_guard<std::mutex> lock(mutex_);
                shared_.downloadNow = now;
                if (total > 0) shared_.downloadTotal = total;
            });
            result = "Saved " + target;
        } catch (const std::exception& error) {
            result = std::string("Download failed: ") + error.what();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        shared_.downloading = false;
        shared_.message = result;
    });
}

void Worker::pollState() {
    try {
        if (!camera_.settings().size()) {
            camera_.loadSchema(/*refresh=*/true);
            std::lock_guard<std::mutex> lock(mutex_);
            shared_.info = camera_.info();
            shared_.modes = camera_.modes();
            shared_.settings = camera_.settings();
            shared_.haveSchema = true;
        }
        gopro::State state = camera_.state();
        std::lock_guard<std::mutex> lock(mutex_);
        shared_.state = std::move(state);
        if (!shared_.connected) shared_.error.clear();
        shared_.connected = true;
    } catch (const gopro::HttpError& error) {
        if (error.cameraBusy()) return;  // still connected, just busy
        previewStarted_ = false;         // the stream must be requested again
        std::lock_guard<std::mutex> lock(mutex_);
        shared_.connected = false;
        shared_.error = error.what();
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_.error = error.what();
    }
}

void Worker::run() {
    auto nextPoll = Clock::now();
    auto nextKeepAlive = Clock::now();

    while (!stop_) {
        std::pair<std::string, Task> job;
        bool haveJob = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_until(lock, std::min(nextPoll, nextKeepAlive),
                             [this] { return stop_ || !tasks_.empty(); });
            if (stop_) break;
            if (!tasks_.empty()) {
                job = std::move(tasks_.front());
                tasks_.pop_front();
                haveJob = true;
            }
        }

        if (haveJob) {
            try {
                job.second(camera_);
                if (!job.first.empty()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    shared_.message = job.first;
                }
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(mutex_);
                shared_.message = std::string("Failed: ") + error.what();
            }
            nextPoll = Clock::now();  // show the effect of the action right away
        }

        const auto now = Clock::now();
        if (now >= nextPoll) {
            pollState();
            nextPoll = Clock::now() + std::chrono::milliseconds(700);
        }

        const bool connected = snapshot().connected;
        if (connected && previewWanted_ && now >= nextKeepAlive) {
            try {
                if (!previewStarted_) {
                    camera_.startStream();
                    previewStarted_ = true;
                }
                camera_.sendStreamKeepAlive();
            } catch (const std::exception&) {
                previewStarted_ = false;
            }
            nextKeepAlive = Clock::now() + std::chrono::milliseconds(2500);
        } else if (now >= nextKeepAlive) {
            nextKeepAlive = Clock::now() + std::chrono::milliseconds(2500);
        }
    }

    if (previewStarted_) {
        try {
            camera_.stopStream();
        } catch (const std::exception&) {
        }
    }
}
