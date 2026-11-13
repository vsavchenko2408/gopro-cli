// Receives the camera's live preview (MPEG-TS over UDP, H.264 video) and
// decodes it on a background thread. The UI thread only asks for the newest
// frame, so nothing is ever queued and the delay stays as low as possible.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class VideoStream {
public:
    explicit VideoStream(int udpPort) : port_(udpPort) {}
    ~VideoStream() { stop(); }
    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    void start();
    void stop();

    // Copies the newest frame (RGBA) if one arrived since the last call.
    bool takeFrame(std::vector<std::uint8_t>& rgba, int& width, int& height);
    // Seconds since the last decoded frame; large when the stream has stalled.
    double secondsSinceFrame() const;
    std::string error() const;

private:
    void run();

    int port_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<std::int64_t> lastFrameMs_{0};

    mutable std::mutex mutex_;
    std::vector<std::uint8_t> frame_;
    int width_ = 0, height_ = 0;
    bool fresh_ = false;
    std::string error_;
};
