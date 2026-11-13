#include "video_stream.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <chrono>

namespace {

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = "";
    av_strerror(code, text, sizeof text);
    return text;
}

}  // namespace

void VideoStream::start() {
    if (thread_.joinable()) return;
    stopRequested_ = false;
    thread_ = std::thread(&VideoStream::run, this);
}

void VideoStream::stop() {
    stopRequested_ = true;
    if (thread_.joinable()) thread_.join();
}

bool VideoStream::takeFrame(std::vector<std::uint8_t>& rgba, int& width, int& height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fresh_) return false;
    rgba = frame_;
    width = width_;
    height = height_;
    fresh_ = false;
    return true;
}

double VideoStream::secondsSinceFrame() const {
    const std::int64_t last = lastFrameMs_;
    return last == 0 ? 1e9 : static_cast<double>(nowMs() - last) / 1000.0;
}

std::string VideoStream::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void VideoStream::run() {
    const auto fail = [this](const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = message;
    };

    // The decoder reports every damaged packet on stderr; a lost UDP packet is
    // routine on Wi-Fi and the picture recovers at the next key frame.
    av_log_set_level(AV_LOG_FATAL);

    AVFormatContext* input = avformat_alloc_context();
    // Lets stop() interrupt a read that is blocked waiting for packets.
    input->interrupt_callback.opaque = this;
    input->interrupt_callback.callback = [](void* self) -> int {
        return static_cast<VideoStream*>(self)->stopRequested_ ? 1 : 0;
    };
    input->flags |= AVFMT_FLAG_NOBUFFER;

    // A generous socket buffer so a busy moment does not drop packets.
    const std::string url = "udp://0.0.0.0:" + std::to_string(port_) +
                            "?overrun_nonfatal=1&fifo_size=100000&buffer_size=2097152";
    const int opened = avformat_open_input(&input, url.c_str(), av_find_input_format("mpegts"), nullptr);
    if (opened < 0) {
        fail("cannot listen on UDP port " + std::to_string(port_) + ": " + avError(opened));
        return;  // avformat_open_input frees the context on failure
    }

    // No avformat_find_stream_info(): it would buffer about a second before
    // the first picture. H.264 carries its own parameters inside the stream,
    // so the decoder can start from the first key frame it sees.
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    decoder->thread_count = 1;  // frame threading would add whole frames of delay
    if (!codec || avcodec_open2(decoder, codec, nullptr) < 0) {
        fail("cannot open the H.264 decoder");
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
        return;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* scaler = nullptr;
    std::vector<std::uint8_t> rgba;

    while (!stopRequested_) {
        const int read = av_read_frame(input, packet);
        if (read < 0) {
            if (stopRequested_) break;
            if (read == AVERROR(EAGAIN)) continue;
            fail("preview stream ended: " + avError(read));
            break;
        }
        const AVStream* stream = input->streams[packet->stream_index];
        if (stream->codecpar->codec_id == AV_CODEC_ID_H264 &&
            avcodec_send_packet(decoder, packet) >= 0) {
            while (avcodec_receive_frame(decoder, frame) >= 0) {
                scaler = sws_getCachedContext(scaler, frame->width, frame->height,
                                              static_cast<AVPixelFormat>(frame->format),
                                              frame->width, frame->height, AV_PIX_FMT_RGBA,
                                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (!scaler) continue;
                rgba.resize(static_cast<std::size_t>(frame->width) * frame->height * 4);
                std::uint8_t* planes[4] = {rgba.data(), nullptr, nullptr, nullptr};
                int strides[4] = {frame->width * 4, 0, 0, 0};
                sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    frame_.swap(rgba);
                    width_ = frame->width;
                    height_ = frame->height;
                    fresh_ = true;
                    error_.clear();
                }
                lastFrameMs_ = nowMs();
            }
        }
        av_packet_unref(packet);
    }

    sws_freeContext(scaler);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
}
