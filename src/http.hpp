// Thin wrapper around libcurl: plain HTTP GET, which is all the camera speaks.
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace gopro {

// Thrown when the camera cannot be reached or answers with an HTTP error.
class HttpError : public std::runtime_error {
public:
    explicit HttpError(const std::string& message, long httpStatus = 0)
        : std::runtime_error(message), httpStatus_(httpStatus) {}

    // HTTP status the camera answered with, or 0 if it did not answer at all.
    long httpStatus() const { return httpStatus_; }
    // 500/503 is how the camera says "busy" (saving a file, switching mode).
    bool cameraBusy() const { return httpStatus_ == 500 || httpStatus_ == 503; }

private:
    long httpStatus_;
};

// Called during a download with (bytes received, total bytes; 0 if unknown).
using ProgressFn = std::function<void(std::uint64_t, std::uint64_t)>;

// Print every request and the camera's answer to stderr (for --verbose).
void setHttpTrace(bool enabled);

// GET the URL and return the response body. Throws HttpError.
std::string httpGet(const std::string& url, long timeoutSeconds = 10);

// GET the URL and stream the body into a file. Throws HttpError.
void httpDownload(const std::string& url, const std::string& filePath,
                  const ProgressFn& progress = {});

}  // namespace gopro
