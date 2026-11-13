#include "http.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <memory>

namespace gopro {
namespace {

bool traceEnabled = false;

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

struct FileCloser {
    void operator()(std::FILE* file) const { std::fclose(file); }
};

using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

CurlHandle makeHandle(const std::string& url) {
    static CurlGlobal global;
    CurlHandle curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw HttpError("cannot initialise libcurl");
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    // The camera is always a direct, local peer: never go through a proxy.
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    return curl;
}

void perform(CURL* curl, const std::string& url) {
    char error[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    const CURLcode rc = curl_easy_perform(curl);
    if (traceEnabled) {
        long code = 0;
        double seconds = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &seconds);
        std::fprintf(stderr, "[http] GET %s -> %s (%.0f ms)\n", url.c_str(),
                     rc == CURLE_OK ? std::to_string(code).c_str() : curl_easy_strerror(rc),
                     seconds * 1000);
    }
    if (rc != CURLE_OK) {
        const std::string detail = error[0] ? error : curl_easy_strerror(rc);
        throw HttpError("no answer from " + url + " (" + detail + ")");
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        throw HttpError("camera answered HTTP " + std::to_string(status) + " for " + url, status);
    }
}

size_t appendToString(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

size_t writeToFile(char* data, size_t size, size_t count, void* user) {
    return std::fwrite(data, size, count, static_cast<std::FILE*>(user));
}

int onProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    const auto& fn = *static_cast<const ProgressFn*>(user);
    fn(static_cast<std::uint64_t>(now), static_cast<std::uint64_t>(total));
    return 0;
}

}  // namespace

void setHttpTrace(bool enabled) {
    traceEnabled = enabled;
}

std::string httpGet(const std::string& url, long timeoutSeconds) {
    CurlHandle curl = makeHandle(url);
    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    try {
        perform(curl.get(), url);
    } catch (...) {
        if (traceEnabled && !body.empty()) std::fprintf(stderr, "[http] body: %s\n", body.c_str());
        throw;
    }
    return body;
}

void httpDownload(const std::string& url, const std::string& filePath,
                  const ProgressFn& progress) {
    // Download into "<name>.part" so an interrupted transfer never looks complete.
    const std::string partPath = filePath + ".part";
    std::unique_ptr<std::FILE, FileCloser> file(std::fopen(partPath.c_str(), "wb"));
    if (!file) throw HttpError("cannot write to " + partPath);

    CurlHandle curl = makeHandle(url);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, file.get());
    // No overall timeout (videos are large); give up if the link stalls for 20 s.
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 20L);
    if (progress) {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &progress);
    }
    try {
        perform(curl.get(), url);
    } catch (...) {
        file.reset();
        std::remove(partPath.c_str());
        throw;
    }
    if (std::fclose(file.release()) != 0 || std::rename(partPath.c_str(), filePath.c_str()) != 0) {
        std::remove(partPath.c_str());
        throw HttpError("cannot finish writing " + filePath);
    }
}

}  // namespace gopro
